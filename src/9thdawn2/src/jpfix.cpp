// jpfix.cpp - 9th Dawn II 日本語表示パッチ
// version.dll のプロキシとして読み込まれ(SDL2.dll 経由)、ninthdawnii.exe の文字描画処理を書き換える。
//
// 文字コード（ゲーム内部用の独自2バイトコード）:
//   1バイト目(リード)  : 0x80-0x9F から 0x8E,0x97 を除いた30種
//   2バイト目(トレイル): 0xA0-0xFF から区切り文字等12種を除いた84種
//   通し番号 index = リード順位*84 + トレイル順位  (最大2520字)
// リードとトレイルは範囲が重ならないので、1バイトだけ見てどちらか判別できる。
//
// 仕組み:
//   * グリフ表参照 [obj+eax*4+0x1538] (128個の配列) を自前関数 getglyph() に置き換える。
//     リード → 幅0の透明グリフ、トレイル → 直前のリードと組み合わせて日本語グリフを返す。
//   * レイアウト関数の「0x80以上は空白扱い」分岐を潰す。
//   * クリップ描画関数 0x54e0d0 をフックし、日本語グリフなら自前テクスチャに差し替える。
//   * 描画関数のテキスト代入をフックし、英語→日本語の辞書で文字列ごと置換する。

#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <share.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "opengl32.lib")
#include "exports.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// ---------------------------------------------------------------- 定数
static const uint32_t IMAGE_BASE = 0x400000;
static const int NTRAIL = 84;
static const int NSLOT = 30 * NTRAIL; // 2520
static const int NVAR = 4;           // 0:19白 1:22白 2:19暗 3:22暗

static int8_t g_leadIdx[256];
static int8_t g_trailIdx[256];
static uint8_t g_leadByte[30];
static uint8_t g_trailByte[NTRAIL];

static void init_code_tables() {
    memset(g_leadIdx, -1, sizeof(g_leadIdx));
    memset(g_trailIdx, -1, sizeof(g_trailIdx));
    int n = 0;
    for (int c = 0x80; c <= 0x9F; c++) {
        if (c == 0x8E || c == 0x97) continue;
        g_leadIdx[c] = (int8_t)n; g_leadByte[n++] = (uint8_t)c;
    }
    static const int excl[] = {0xA7, 0xAB, 0xC5, 0xC6, 0xCC, 0xD1, 0xE8, 0xE9, 0xEF, 0xF4, 0xF6, 0xFF};
    n = 0;
    for (int c = 0xA0; c <= 0xFF; c++) {
        bool skip = false;
        for (int e : excl) if (c == e) skip = true;
        if (skip) continue;
        g_trailIdx[c] = (int8_t)n; g_trailByte[n++] = (uint8_t)c;
    }
}

// ---------------------------------------------------------------- ログ
static char g_dir[1024];        // <game>\jp\  (末尾\付き、UTF-8。SDL とログ用)
static wchar_t g_dirW[1024];    // 同じパスの UTF-16 (ファイルを開く用)

// jp フォルダ内のファイルを開く。パスに日本語などが含まれても開けるよう UTF-16 で扱う
static FILE* open_jp(const char* name, const wchar_t* mode) {
    wchar_t wname[256], path[1280];
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256)) return nullptr;
    swprintf_s(path, L"%s%s", g_dirW, wname);
    return _wfsopen(path, mode, _SH_DENYNO);
}
static FILE* g_log = nullptr;
static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- ゲーム構造体
// テクスチャクリップ(グリフ) 0x4c バイト
#pragma pack(push, 1)
struct Clip {
    int32_t x, y, w, h;      // 0x00
    int32_t hw, hh;          // 0x10
    int32_t x2, y2;          // 0x18
    int16_t texSize;         // 0x20
    int16_t pad0;
    int32_t pad1[5];         // 0x24
    int32_t frames;          // 0x38 (>1 でアニメーション扱い)
    uint8_t flag3c;          // 0x3c
    uint8_t pad2[11];
    void* texObj;            // 0x48
};
#pragma pack(pop)
static_assert(sizeof(Clip) == 0x4c, "Clip size");

// 偽テクスチャオブジェクト: +0 word=サイズ(正方形), +0x44=GLテクスチャID
struct FakeTex { uint8_t raw[0x60]; };
static FakeTex g_tex[2];
static int g_texState[2]; // 0=未ロード 1=成功 -1=失敗

struct VarInfo { int tex, ox, oy, cw, ch, px, py, cols; };
static VarInfo g_var[NVAR];
static int g_atlasSize = 2048;
static int g_leadX = 4, g_leadY = 2044;

static Clip g_pool[NVAR][NSLOT + 1]; // 最後の1つはリード用の透明グリフ
static uint32_t g_poolSize = sizeof(g_pool);
static bool g_fontReady = false;

static void init_pool() {
    for (int v = 0; v < NVAR; v++) {
        VarInfo& vi = g_var[v];
        void* tex = &g_tex[vi.tex];
        for (int i = 0; i <= NSLOT; i++) {
            Clip& c = g_pool[v][i];
            memset(&c, 0, sizeof(c));
            if (i < NSLOT) {
                c.x = vi.ox + (i % vi.cols) * vi.px;
                c.y = vi.oy + (i / vi.cols) * vi.py;
            } else {
                c.x = g_leadX; c.y = g_leadY;
            }
            c.w = vi.cw; c.h = vi.ch;
            c.hw = c.w / 2; c.hh = c.h / 2;
            c.x2 = c.x + c.w; c.y2 = c.y + c.h;
            c.texSize = (int16_t)g_atlasSize;
            c.frames = 0;
            c.texObj = tex;
        }
        // リードは幅0のUV
        Clip& L = g_pool[v][NSLOT];
        L.x2 = L.x; L.y2 = L.y;
    }
    for (int t = 0; t < 2; t++) {
        memset(&g_tex[t], 0, sizeof(FakeTex));
        *(int16_t*)(g_tex[t].raw + 0) = (int16_t)g_atlasSize;
    }
}

// ---------------------------------------------------------------- グリフ取得
static int g_lastLead = 0;

static int variant_of(const char* obj) {
    const char* gA = *(const char* const*)(obj + 0x1538 + 'A' * 4);
    if (!gA) return -1;
    int x = *(const int*)gA;
    int h = *(const int*)(gA + 0xc);
    int size = (h <= 24) ? 0 : 1;
    int dark = (x >= 484) ? 1 : 0; // HUD.png 上の位置で白/暗フォントを判別
    return dark * 2 + size;
}

extern "C" void* __cdecl getglyph(char* obj, int idx) {
    unsigned c = (unsigned)idx & 0xFF;
    void** table = (void**)(obj + 0x1538);
    if (c < 0x80) return table[c];
    if (!g_fontReady) return table['?'];
    int v = variant_of(obj);
    if (v < 0) return table['?'];
    int li = g_leadIdx[c];
    if (li >= 0) {
        g_lastLead = li;
        Clip* L = &g_pool[v][NSLOT];
        L->w = -*(int*)(obj + 0x144c); // 字間を打ち消して送り幅0にする
        return L;
    }
    int ti = g_trailIdx[c];
    if (ti >= 0) return &g_pool[v][g_lastLead * NTRAIL + ti];
    return table['?'];
}

// ---------------------------------------------------------------- テクスチャ読み込み
struct SDL_PixelFormat_ { uint32_t format; void* palette; uint8_t BitsPerPixel, BytesPerPixel, pad[2]; uint32_t Rmask, Gmask, Bmask, Amask; };
struct SDL_Surface_ { uint32_t flags; SDL_PixelFormat_* format; int w, h, pitch; void* pixels; };
typedef SDL_Surface_* (__cdecl* IMG_Load_t)(const char*);
typedef void (__cdecl* SDL_FreeSurface_t)(SDL_Surface_*);
static IMG_Load_t p_IMG_Load;
static SDL_FreeSurface_t p_SDL_FreeSurface;

static void load_texture(int t) {
    g_texState[t] = -1;
    char path[1280];
    sprintf_s(path, "%s%s", g_dir, t == 0 ? "font_w.png" : "font_d.png");
    if (!p_IMG_Load) {
        HMODULE orig = GetModuleHandleA("SDL2_image.dll");
        if (orig) p_IMG_Load = (IMG_Load_t)GetProcAddress(orig, "IMG_Load");
    }
    if (!p_SDL_FreeSurface) {
        HMODULE sdl = GetModuleHandleA("SDL2.dll");
        if (sdl) p_SDL_FreeSurface = (SDL_FreeSurface_t)GetProcAddress(sdl, "SDL_FreeSurface");
    }
    if (!p_IMG_Load || !p_SDL_FreeSurface) { logf("IMG_Load 未取得"); return; }
    SDL_Surface_* s = p_IMG_Load(path);
    if (!s) { logf("テクスチャ読込失敗: %s", path); return; }
    int w = s->w, h = s->h;
    std::vector<uint8_t> px((size_t)w * h * 4);
    SDL_PixelFormat_* f = s->format;
    if (f->BytesPerPixel != 4) { logf("想定外の画素形式 bpp=%d", f->BytesPerPixel); p_SDL_FreeSurface(s); return; }
    auto shift = [](uint32_t m) { int n = 0; while (m && !(m & 1)) { m >>= 1; n++; } return n; };
    int rs = shift(f->Rmask), gs = shift(f->Gmask), bs = shift(f->Bmask), as = shift(f->Amask);
    for (int y = 0; y < h; y++) {
        const uint32_t* src = (const uint32_t*)((const uint8_t*)s->pixels + (size_t)y * s->pitch);
        uint8_t* dst = &px[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            dst[x * 4 + 0] = (uint8_t)(p >> rs);
            dst[x * 4 + 1] = (uint8_t)(p >> gs);
            dst[x * 4 + 2] = (uint8_t)(p >> bs);
            dst[x * 4 + 3] = f->Amask ? (uint8_t)(p >> as) : 255;
        }
    }
    p_SDL_FreeSurface(s);
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    *(uint32_t*)(g_tex[t].raw + 0x44) = id;
    g_texState[t] = 1;
    logf("テクスチャ読込: %s (%dx%d) id=%u", path, w, h, id);
}

extern "C" void __cdecl ensure_texture(FakeTex* t) {
    int i = (int)(t - g_tex);
    if (i < 0 || i > 1) return;
    if (g_texState[i] == 0) load_texture(i);
}

// ---------------------------------------------------------------- 文字列辞書
static std::unordered_map<std::string, std::string> g_dict;
static std::unordered_map<uint32_t, uint16_t> g_charmap; // Unicode -> index
static std::unordered_set<std::string> g_seen;
static FILE* g_untr = nullptr;
static bool g_dumpUntranslated = true;
static std::string g_testPrefix; // config.ini の test_prefix (エンコード済み)

static std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == 'n') o += '\n';
            else if (n == 't') o += '\t';
            else if (n == 'r') o += '\r';
            else if (n == '\\') o += '\\';
            else if (n == 'x' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
            else { o += '\\'; o += n; }
        } else o += s[i];
    }
    return o;
}

static std::string escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20 || c >= 0x80) { char b[8]; sprintf_s(b, "\\x%02X", c); o += b; }
        else o += (char)c;
    }
    return o;
}

// UTF-8(エスケープ済み) → ゲーム内部コード
static int g_missingChars = 0;
static std::string encode_ja(const std::string& u) {
    std::string o;
    size_t i = 0;
    while (i < u.size()) {
        unsigned char c = (unsigned char)u[i];
        if (c == '\\' && i + 1 < u.size()) { // エスケープ
            char n = u[i + 1];
            if (n == 'x' && i + 3 < u.size()) { o += (char)strtol(u.substr(i + 2, 2).c_str(), nullptr, 16); i += 4; continue; }
            if (n == 'n') { o += '\n'; i += 2; continue; }
            if (n == 't') { o += '\t'; i += 2; continue; }
            if (n == 'r') { o += '\r'; i += 2; continue; }
            if (n == '\\') { o += '\\'; i += 2; continue; }
        }
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < u.size(); k++) cp = (cp << 6) | ((unsigned char)u[i + k] & 0x3F);
        i += len;
        if (cp < 0x80) { o += (char)cp; continue; }
        auto it = g_charmap.find(cp);
        if (it == g_charmap.end()) { o += '?'; g_missingChars++; continue; }
        int idx = it->second;
        o += (char)g_leadByte[idx / NTRAIL];
        o += (char)g_trailByte[idx % NTRAIL];
    }
    return o;
}

static bool read_lines(const char* name, std::vector<std::string>& lines) {
    FILE* f = open_jp(name, L"rb");
    if (!f) return false;
    std::string cur; int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); lines.push_back(cur); cur.clear(); }
        else cur += (char)ch;
    }
    if (!cur.empty()) lines.push_back(cur);
    fclose(f);
    if (!lines.empty() && lines[0].compare(0, 3, "\xEF\xBB\xBF") == 0) lines[0].erase(0, 3); // BOM
    return true;
}

static bool load_font_info() {
    std::vector<std::string> lines;
    if (!read_lines("font.ini", lines)) { logf("font.ini がありません"); return false; }
    int got = 0;
    for (auto& l : lines) {
        int v; VarInfo vi;
        if (sscanf_s(l.c_str(), "var%d=%d,%d,%d,%d,%d,%d,%d,%d", &v, &vi.tex, &vi.ox, &vi.oy, &vi.cw, &vi.ch, &vi.px, &vi.py, &vi.cols) == 9 && v >= 0 && v < NVAR) { g_var[v] = vi; got |= 1 << v; }
        sscanf_s(l.c_str(), "atlas=%d", &g_atlasSize);
        sscanf_s(l.c_str(), "lead=%d,%d", &g_leadX, &g_leadY);
    }
    if (got != 0xF) { logf("font.ini の var 定義が不足 (%x)", got); return false; }
    return true;
}

static void load_charmap() {
    std::vector<std::string> lines;
    if (!read_lines("charmap.txt", lines)) { logf("charmap.txt がありません"); return; }
    for (auto& l : lines) {
        // 形式: index<TAB>文字
        size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        int idx = atoi(l.c_str());
        std::string u = l.substr(tab + 1);
        if (u.empty() || idx < 0 || idx >= NSLOT) continue;
        unsigned char c = (unsigned char)u[0];
        uint32_t cp; int len;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else continue;
        for (int k = 1; k < len && k < (int)u.size(); k++) cp = (cp << 6) | ((unsigned char)u[k] & 0x3F);
        g_charmap[cp] = (uint16_t)idx;
    }
    logf("charmap: %u 字", (unsigned)g_charmap.size());
}

// パターン: 英語側に {0}..{9} / {t0}..{t9} を含む辞書項目。
//   {n}  任意の1文字以上に一致。一致部分が辞書にあれば訳して埋め込む。
//   {tn} 辞書にある語にだけ一致 (汎用的な "{t0}: {1}" などを安全に使うため)。
// 日本語側では {n} と {tn} のどちらで書いても同じ番号として扱う。
struct Pattern {
    std::vector<std::string> lits; // リテラル (キャプチャ数+1 個)
    std::vector<int> capNo;        // 各キャプチャの番号
    std::vector<bool> capDict;     // 辞書語限定か
    std::string ja;                // エンコード済み、{n} を含む
};
static std::vector<Pattern> g_patterns;

// s[i] からの "{n}" / "{tn}" を解析。成功なら長さを返し、番号と種別を設定
static size_t placeholder_at(const std::string& s, size_t i, int& no, bool& dict) {
    if (s[i] != '{') return 0;
    size_t j = i + 1;
    dict = false;
    if (j < s.size() && s[j] == 't') { dict = true; j++; }
    if (j + 1 < s.size() && s[j] >= '0' && s[j] <= '9' && s[j + 1] == '}') { no = s[j] - '0'; return j + 2 - i; }
    return 0;
}

static bool parse_pattern(const std::string& en, Pattern& p) {
    p.lits.assign(1, std::string());
    for (size_t i = 0; i < en.size(); i++) {
        int no; bool dict;
        size_t n = placeholder_at(en, i, no, dict);
        if (n) {
            p.capNo.push_back(no);
            p.capDict.push_back(dict);
            p.lits.push_back(std::string());
            i += n - 1;
        } else p.lits.back() += en[i];
    }
    return !p.capNo.empty();
}

// 配布用の辞書は英語原文を含めないよう、キーを原文の FNV-1a 64bit ハッシュにしている
// (行頭 '@' + 16桁の16進)。開発用の英語キーの辞書とどちらでも引けるようにする。
static std::unordered_map<uint64_t, std::string> g_hdict;

static uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

static const std::string* dict_find(const std::string& k) {
    auto it = g_dict.find(k);
    if (it != g_dict.end()) return &it->second;
    if (!g_hdict.empty()) {
        auto h = g_hdict.find(fnv1a(k));
        if (h != g_hdict.end()) return &h->second;
    }
    return nullptr;
}

static void load_dict() {
    std::vector<std::string> lines;
    if (!read_lines("strings.txt", lines)) { logf("strings.txt がありません"); return; }
    for (auto& l : lines) {
        if (l.empty() || l[0] == '#') continue;
        size_t tab = l.find('\t');
        if (tab == std::string::npos) continue;
        if (l[0] == '@' && tab == 17) {
            std::string ja = l.substr(tab + 1);
            if (!ja.empty()) g_hdict[_strtoui64(l.substr(1, 16).c_str(), nullptr, 16)] = encode_ja(ja);
            continue;
        }
        std::string en = unescape(l.substr(0, tab));
        std::string ja = l.substr(tab + 1);
        if (ja.empty() || en.empty()) continue;
        Pattern p;
        if (parse_pattern(en, p)) { p.ja = encode_ja(ja); g_patterns.push_back(std::move(p)); }
        else g_dict[en] = encode_ja(ja);
    }
    // リテラルが長いパターンほど優先
    std::stable_sort(g_patterns.begin(), g_patterns.end(), [](const Pattern& a, const Pattern& b) {
        size_t la = 0, lb = 0;
        for (auto& s : a.lits) la += s.size();
        for (auto& s : b.lits) lb += s.size();
        return la > lb;
    });
    logf("辞書: %u 件 / パターン %u 件 (未収録文字 %d)", (unsigned)(g_dict.size() + g_hdict.size()), (unsigned)g_patterns.size(), g_missingChars);
}

static bool match_pattern(const Pattern& p, const std::string& s, std::vector<std::string>& caps) {
    const auto& L = p.lits;
    if (s.compare(0, L[0].size(), L[0]) != 0) return false;
    size_t pos = L[0].size();
    caps.clear();
    for (size_t i = 1; i < L.size(); i++) {
        const std::string& lit = L[i];
        size_t f;
        if (i == L.size() - 1) {
            if (s.size() < pos + lit.size() + 1) return false;
            f = s.size() - lit.size();
            if (s.compare(f, lit.size(), lit) != 0) return false;
        } else {
            f = s.find(lit, pos + 1);
            if (f == std::string::npos || lit.empty()) return false;
        }
        if (f <= pos) return false;
        if (s.find('\n', pos) < f) return false; // キャプチャは改行をまたがない
        caps.push_back(s.substr(pos, f - pos));
        pos = f + lit.size();
    }
    return true;
}

static bool has_ja(const std::string& s) {
    for (unsigned char c : s) if (g_leadIdx[c] >= 0) return true;
    return false;
}

// 先頭の色指定コード «n ïn ïn ïn ïn ï の並びの長さ
static size_t color_prefix_len(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (unsigned char)s[i] == 0xAB) {
        size_t j = i + 1;
        int fields = 0;
        while (fields < 5) {
            size_t k = j;
            if (k < s.size() && s[k] == '-') k++;
            size_t d = k;
            while (k < s.size() && s[k] >= '0' && s[k] <= '9') k++;
            if (k == d || k >= s.size() || (unsigned char)s[k] != 0xEF) break;
            j = k + 1; fields++;
        }
        if (fields < 5) break;
        i = j;
    }
    return i;
}

static bool lookup(const std::string& body, std::string& out) {
    if (const std::string* d = dict_find(body)) { out = *d; return true; }
    std::vector<std::string> caps;
    for (auto& p : g_patterns) {
        if (!match_pattern(p, body, caps)) continue;
        bool ok = true;
        for (size_t c = 0; c < caps.size(); c++)
            if (p.capDict[c] && !dict_find(caps[c])) { ok = false; break; }
        if (!ok) continue;
        std::string r;
        const std::string& j = p.ja;
        for (size_t i = 0; i < j.size(); i++) {
            int n; bool dict;
            size_t len = placeholder_at(j, i, n, dict);
            if (len) {
                for (size_t c = 0; c < p.capNo.size(); c++) {
                    if (p.capNo[c] != n) continue;
                    const std::string* d = dict_find(caps[c]);
                    r += d ? *d : caps[c];
                    break;
                }
                i += len - 1;
            } else r += j[i];
        }
        out = r;
        return true;
    }
    return false;
}

static void log_untranslated(const std::string& body);

static bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// 前後の空白・改行を外して照合
static bool lookup_trim(const std::string& s, std::string& out) {
    if (lookup(s, out)) return true;
    size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) a++;
    while (b > a && is_ws(s[b - 1])) b--;
    if (a == b || (a == 0 && b == s.size())) return false;
    std::string t;
    if (!lookup(s.substr(a, b - a), t)) return false;
    out = s.substr(0, a) + t + s.substr(b);
    return true;
}

// 全体 → 前後の空白除去 → 色コード/改行で区切った部分ごと、の順で翻訳を試みる
static std::string g_qOpen, g_qClose; // 「」(エンコード済み)

static bool translate_text(const std::string& s, std::string& out) {
    if (lookup_trim(s, out)) return true;
    // 会話は表示時に "..." で囲まれるので、外して訳し「」で囲み直す
    if (s.size() >= 3 && s.front() == '"' && s.back() == '"' && !g_qOpen.empty()) {
        std::string t;
        if (translate_text(s.substr(1, s.size() - 2), t)) { out = g_qOpen + t + g_qClose; return true; }
    }
    std::string r, piece;
    bool any = false, split = false;
    std::vector<std::string> missed;
    auto flush = [&]() {
        if (piece.empty()) return;
        std::string t;
        bool hasWord = false;
        for (char ch : piece) if (isalpha((unsigned char)ch)) { hasWord = true; break; }
        if (hasWord && lookup_trim(piece, t)) { r += t; any = true; }
        else { r += piece; if (hasWord) missed.push_back(piece); }
        piece.clear();
    };
    for (size_t i = 0; i < s.size();) {
        size_t cl = (unsigned char)s[i] == 0xAB ? color_prefix_len(s.substr(i)) : 0;
        if (cl) { flush(); r += s.substr(i, cl); i += cl; split = true; continue; }
        if (s[i] == '\n') { flush(); r += '\n'; i++; split = true; continue; }
        piece += s[i++];
    }
    flush();
    if (!split) return false;
    for (auto& m : missed) log_untranslated(m);
    if (!any) return false;
    out = r;
    return true;
}

typedef void (__fastcall* StrAssignPtr_t)(void* self, void* edx, const char* p, size_t n); // 0x403150 (thiscall)
static StrAssignPtr_t p_strAssign;

// 原文 → 結果 のキャッシュ (未訳は "\x01")
static std::unordered_map<std::string, std::string> g_cache;

static void log_untranslated(const std::string& body) {
    if (!g_dumpUntranslated || !g_untr || g_seen.size() >= 100000) return;
    // 英字2文字以上の並びを含むものだけ記録 (時刻・数値などを除く)
    bool ok = false;
    for (size_t i = 0; i + 1 < body.size(); i++)
        if (isalpha((unsigned char)body[i]) && isalpha((unsigned char)body[i + 1])) { ok = true; break; }
    if (!ok || has_ja(body)) return;
    if (g_seen.insert(body).second) {
        fprintf(g_untr, "%s\t\n", escape(body).c_str());
        fflush(g_untr);
    }
}

// 表示用の訳を返す (訳がなければ nullptr)。結果はキャッシュする。
static const std::string* get_translation(const std::string& key) {
    if (key.empty()) return nullptr;
    auto c = g_cache.find(key);
    if (c == g_cache.end()) {
        std::string result = "\x01";
        if (!has_ja(key)) {
            size_t cp = color_prefix_len(key);
            std::string body = key.substr(cp);
            std::string tr;
            if (translate_text(body, tr)) result = key.substr(0, cp) + tr;
            else {
                log_untranslated(body);
                if (!g_testPrefix.empty()) result = key.substr(0, cp) + g_testPrefix + body;
            }
        }
        if (g_cache.size() > 50000) g_cache.clear();
        c = g_cache.emplace(key, result).first;
    }
    return c->second != "\x01" ? &c->second : nullptr;
}

extern "C" void __cdecl translate_inplace(char* s) {
    uint32_t size = *(uint32_t*)(s + 0x10);
    uint32_t cap = *(uint32_t*)(s + 0x14);
    const char* p = cap >= 16 ? *(const char**)s : s;
    const std::string* t = get_translation(std::string(p, size));
    if (t) p_strAssign(s, nullptr, t->data(), t->size());
}

// ---------------------------------------------------------------- ランチャー
// ランチャーは SDL_Surface に直接描く別のフォント処理を使う。
//   フォント構造 font(=launcher+0x344): +0 SDL_Surface*, +0xc+c*16 SDL_Rect(グリフ),
//   +0x80c 空白幅, +0x814 文字高さ。描画先サーフェスは launcher+0x20。
// 描画関数(0x4ff310)と幅計算(0x4fcae0)を丸ごと置き換え、日本語は font_w.png から描く。
struct MsStr { union { char buf[16]; char* ptr; }; uint32_t size, cap; }; // MSVC2013 std::string
typedef void (__cdecl* op_delete_t)(void*);
typedef int (__cdecl* SDL_UpperBlit_t)(void* src, const void* srcrect, void* dst, void* dstrect);
static uintptr_t a_opDeleteIat; // 0x5b1218 (MSVCR120 operator delete の IAT)
static SDL_UpperBlit_t p_upperBlit;
static SDL_Surface_* g_lsurf;
static int g_lsurfState;

static void ms_free(MsStr* s) {
    if (s->cap >= 16) (*(op_delete_t*)a_opDeleteIat)(s->ptr);
}

static std::string launcher_text(const MsStr* s) {
    std::string k(s->cap >= 16 ? s->ptr : s->buf, s->size);
    const std::string* t = get_translation(k);
    return t ? *t : k;
}

static int launcher_width(const char* font, const std::string& s) {
    int w = 0;
    for (unsigned char c : s) {
        if (c < 0x80) w += *(const int*)(font + 0x14 + c * 16);
        else if (g_leadIdx[c] >= 0) {}
        else if (g_trailIdx[c] >= 0) w += g_var[1].cw;
        else w += *(const int*)(font + 0x80c);
    }
    return w;
}

static SDL_Surface_* launcher_surface() {
    if (g_lsurfState) return g_lsurf;
    g_lsurfState = 1;
    if (!p_IMG_Load) {
        HMODULE orig = GetModuleHandleA("SDL2_image.dll");
        if (orig) p_IMG_Load = (IMG_Load_t)GetProcAddress(orig, "IMG_Load");
    }
    char path[1280];
    sprintf_s(path, "%sfont_w.png", g_dir);
    g_lsurf = p_IMG_Load ? p_IMG_Load(path) : nullptr;
    logf(g_lsurf ? "ランチャー用フォント読込: %s" : "ランチャー用フォント読込失敗: %s", path);
    return g_lsurf;
}

extern "C" int __stdcall launcher_width_impl(char* font, MsStr* s) {
    std::string t = launcher_text(s);
    ms_free(s);
    return launcher_width(font, t);
}

extern "C" void __stdcall launcher_draw_impl(char* self, int x, int y, int align, MsStr* s) {
    std::string t = launcher_text(s);
    ms_free(s);
    if (!p_upperBlit) {
        HMODULE sdl = GetModuleHandleA("SDL2.dll");
        if (sdl) p_upperBlit = (SDL_UpperBlit_t)GetProcAddress(sdl, "SDL_UpperBlit");
        if (!p_upperBlit) return;
    }
    char* font = self + 0x344;
    int w = launcher_width(font, t);
    if (align == 0) x -= w / 2;
    else if (align == 1) x -= w;
    y -= *(int*)(font + 0x814) / 2;
    void* fsurf = *(void**)font;
    void* dst = *(void**)(self + 0x20);
    int lead = 0;
    for (unsigned char c : t) {
        if (c < 0x80) {
            const int* r = (const int*)(font + 0xc + c * 16);
            if (fsurf) { int d[4] = {x, y, r[2], r[3]}; p_upperBlit(fsurf, r, dst, d); }
            x += r[2];
        } else if (g_leadIdx[c] >= 0) {
            lead = g_leadIdx[c];
        } else if (g_trailIdx[c] >= 0) {
            const VarInfo& v = g_var[1];
            int idx = lead * NTRAIL + g_trailIdx[c];
            SDL_Surface_* js = g_fontReady ? launcher_surface() : nullptr;
            if (js) {
                int r[4] = {v.ox + (idx % v.cols) * v.px, v.oy + (idx / v.cols) * v.py, v.cw, v.ch};
                int d[4] = {x, y, v.cw, v.ch};
                p_upperBlit(js, r, dst, d);
            }
            x += v.cw;
        } else {
            x += *(int*)(font + 0x80c);
        }
    }
}

// 0x4fcae0 の置き換え: thiscall(ecx=font, std::string 値渡し) ret 0x18
__declspec(naked) void hook_lwidth() {
    __asm {
        lea eax, [esp + 4]
        push eax
        push ecx
        call launcher_width_impl
        ret 0x18
    }
}

// 0x4ff310 の置き換え: thiscall(ecx=launcher, x, y, align, std::string 値渡し) ret 0x24
__declspec(naked) void hook_ldraw() {
    __asm {
        lea eax, [esp + 0x10]
        push eax
        push dword ptr [esp + 0x10]
        push dword ptr [esp + 0x10]
        push dword ptr [esp + 0x10]
        push ecx
        call launcher_draw_impl
        ret 0x24
    }
}

// ---------------------------------------------------------------- フック本体(naked)
static uintptr_t g_delta;
static uintptr_t a_strAssignSub;   // 0x402dc0
static uintptr_t a_drawClipCont;   // 0x54e0d6
static uintptr_t g_tmp, g_ret;

// call 0x402dc0 の置き換え (thiscall, 引数3, ret 0xc)
__declspec(naked) void hook_assign() {
    __asm {
        push ecx
        push dword ptr [esp + 0x10]
        push dword ptr [esp + 0x10]
        push dword ptr [esp + 0x10]
        call dword ptr [a_strAssignSub]
        pop ecx
        push eax
        push ecx
        call translate_inplace
        add esp, 4
        pop eax
        ret 0xc
    }
}

// 0x54e0d0 先頭の置き換え: 日本語グリフなら第2引数(テクスチャ)を差し替える
__declspec(naked) void hook_drawclip() {
    __asm {
        mov eax, dword ptr [esp + 4]
        sub eax, offset g_pool
        cmp eax, dword ptr [g_poolSize]
        jae orig
        mov eax, dword ptr [esp + 4]
        mov eax, dword ptr [eax + 0x48]
        mov dword ptr [esp + 8], eax
        cmp dword ptr [eax + 0x44], 0
        jne orig
        pushad
        push eax
        call ensure_texture
        add esp, 4
        popad
    orig:
        push ebp
        mov ebp, esp
        mov edx, dword ptr [ebp + 8]
        jmp dword ptr [a_drawClipCont]
    }
}

// ---------------------------------------------------------------- 禁則処理
// 折り返しレイアウト(0x4a8ce0)は空白の位置を改行候補として記録し、行幅を超えたら
// そこまで戻って改行する。日本語文字の直後にも候補を作ることで文字単位の折り返しにし、
// 行頭禁則文字の前・行末禁則文字の後には候補を作らないことで禁則(追い出し)を実現する。
static bool g_kHead[NSLOT]; // 行頭に置かない
static bool g_kTail[NSLOT]; // 行末に置かない

static void init_kinsoku() {
    const char* head = u8"、。，．・：；？！ー～〜…‥）」』】〉》〕ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ々";
    const char* tail = u8"「『（【〈《〔";
    auto mark = [](const char* s, bool* tbl) {
        std::string u(s);
        for (size_t i = 0; i < u.size();) {
            unsigned char c = (unsigned char)u[i];
            int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
            for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)u[i + k] & 0x3F);
            i += len;
            auto it = g_charmap.find(cp);
            if (it != g_charmap.end()) tbl[it->second] = true;
        }
    };
    mark(head, g_kHead);
    mark(tail, g_kTail);
}

typedef void* (__fastcall* StrAssignSub_t)(void* self, void* edx, const void* src, size_t pos, size_t n); // 0x402dc0
static StrAssignSub_t p_strAssignSub;
static uintptr_t a_wrapSpace; // 0x4a90ed 空白の処理
static uintptr_t a_wrapCont;  // 0x4a9191 次の文字へ

extern "C" void __cdecl wrap_candidate(char* obj, const unsigned char* str, int idx) {
    if (idx < 1) return;
    int ti = g_trailIdx[str[idx]];
    int li = g_leadIdx[str[idx - 1]];
    if (ti < 0 || li < 0) return; // 日本語文字の2バイト目のときだけ
    if (g_kTail[li * NTRAIL + ti]) return;
    int len = *(int*)(obj + 0x2a0);
    int n = idx + 1;
    if (n >= len) return;
    unsigned char nc = str[n];
    if (nc == '\n' || nc == ' ') return;
    if (g_leadIdx[nc] >= 0) {
        if (n + 1 < len) {
            int t2 = g_trailIdx[str[n + 1]];
            if (t2 >= 0 && g_kHead[g_leadIdx[nc] * NTRAIL + t2]) return;
        }
    } else if (nc < 0x80 && strchr(",.!?):;]}%", nc)) return;
    int line = *(int*)obj;
    char* lineStr = obj + (0x38 + line) * 24;
    if (*(uint32_t*)(lineStr + 0x10) == 0) return; // 改行直後の空行では作らない
    p_strAssignSub(obj + 0x528, nullptr, lineStr, 0, (size_t)-1);
    *(int*)(obj + 0x1440) = idx;
    *(int*)(obj + 0x1444) = *(int*)(obj + 8 + line * 4);
}

// 0x4a90e3: cmp byte ptr [eax+edi],0x20 / jne 0x4a9191 の置き換え (esi=レイアウト対象)
__declspec(naked) void hook_wrap() {
    __asm {
        cmp byte ptr [eax + edi], 0x20
        je space
        pushad
        push edi
        push eax
        push esi
        call wrap_candidate
        add esp, 12
        popad
        jmp dword ptr [a_wrapCont]
    space:
        jmp dword ptr [a_wrapSpace]
    }
}

// 文字送り(タイプライター)の打ち切り判定。表示可能文字数を2倍にして比較する。
// 日本語は1文字2バイトなので、英語と同じ速さで送られ、原文長で止まっても末尾が欠けない。
// 元: mov ecx,[ebx+0x145c]; mov eax,[eax+8]; cmp ecx,eax
__declspec(naked) void hook_typeA() {
    __asm {
        mov ecx, dword ptr [ebx + 0x145c]
        mov eax, dword ptr [eax + 8]
        add eax, eax
        cmp ecx, eax
        ret
    }
}
// 元: mov eax,[esi+0x145c]; cmp eax,[ecx+8]
__declspec(naked) void hook_typeB() {
    __asm {
        mov eax, dword ptr [esi + 0x145c]
        push edx
        mov edx, dword ptr [ecx + 8]
        add edx, edx
        cmp eax, edx
        pop edx
        ret
    }
}

// ---------------------------------------------------------------- パッチ
static bool check_bytes(uintptr_t va, const char* hex) {
    const uint8_t* p = (const uint8_t*)(va + g_delta);
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned b; sscanf_s(hex + i * 2, "%2x", &b);
        if (p[i] != (uint8_t)b) { logf("バイト不一致 @%08X", (unsigned)va); return false; }
    }
    return true;
}

static void write_mem(uintptr_t addr, const void* src, size_t n) {
    DWORD old;
    VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, src, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
}

static void write_call(uintptr_t at, uintptr_t target, size_t total, uint8_t op = 0xE8) {
    uint8_t buf[16];
    buf[0] = op;
    *(int32_t*)(buf + 1) = (int32_t)(target - (at + 5));
    for (size_t i = 5; i < total; i++) buf[i] = 0x90;
    write_mem(at, buf, total);
}

static uint8_t* g_stubMem = nullptr;
static size_t g_stubUsed = 0;

enum Kind { K_MOV, K_CMP, K_PUSH };
// pushad 後のスタック上オフセット
static int pushad_off(char reg) {
    switch (reg) { case 'a': return 0x1C; case 'c': return 0x18; case 'd': return 0x14; case 'b': return 0x10; }
    return -1;
}

static uintptr_t make_stub(Kind k, uint8_t pushBaseOp, char dst) {
    uint8_t* s = g_stubMem + g_stubUsed;
    uint8_t* p = s;
    *p++ = 0x9C;            // pushfd
    *p++ = 0x60;            // pushad
    *p++ = 0x50;            // push eax (idx)
    *p++ = pushBaseOp;      // push base (obj)
    *p++ = 0xB8; *(uint32_t*)p = (uint32_t)(uintptr_t)&getglyph; p += 4; // mov eax, getglyph
    *p++ = 0xFF; *p++ = 0xD0; // call eax
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08; // add esp, 8
    if (k == K_MOV) {
        *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = (uint8_t)pushad_off(dst); // mov [esp+off], eax
        *p++ = 0x61; *p++ = 0x9D; *p++ = 0xC3; // popad; popfd; ret
    } else {
        *p++ = 0xA3; *(uint32_t*)p = (uint32_t)(uintptr_t)&g_tmp; p += 4; // mov [g_tmp], eax
        *p++ = 0x61; *p++ = 0x9D; // popad; popfd
        if (k == K_CMP) {
            *p++ = 0x83; *p++ = 0x3D; *(uint32_t*)p = (uint32_t)(uintptr_t)&g_tmp; p += 4; *p++ = 0x00; // cmp dword [g_tmp], 0
            *p++ = 0xC3;
        } else {
            *p++ = 0x8F; *p++ = 0x05; *(uint32_t*)p = (uint32_t)(uintptr_t)&g_ret; p += 4; // pop [g_ret]
            *p++ = 0xFF; *p++ = 0x35; *(uint32_t*)p = (uint32_t)(uintptr_t)&g_tmp; p += 4; // push [g_tmp]
            *p++ = 0xFF; *p++ = 0x25; *(uint32_t*)p = (uint32_t)(uintptr_t)&g_ret; p += 4; // jmp [g_ret]
        }
    }
    g_stubUsed += (p - s + 15) & ~15;
    return (uintptr_t)s;
}

struct GlyphPatch { uint32_t va; const char* bytes; Kind kind; char base; char dst; };
static const GlyphPatch kGlyphPatches[] = {
    {0x4a8b86, "8b948638150000",   K_MOV,  'S', 'd'}, // layout(折返し無)
    {0x4a8f4d, "8b948638150000",   K_MOV,  'S', 'd'}, // layout(折返し有)
    {0x4a952d, "8b848638150000",   K_MOV,  'S', 'a'}, // 等幅描画
    {0x4a962c, "ffb48638150000",   K_PUSH, 'S', 0},
    {0x4a9a77, "83bc833815000000", K_CMP,  'B', 0},   // 折返し描画
    {0x4a9a85, "8b848338150000",   K_MOV,  'B', 'a'},
    {0x4a9bef, "ffb48338150000",   K_PUSH, 'B', 0},
    {0x4a9c27, "ffb48338150000",   K_PUSH, 'B', 0},
    {0x4a9c50, "8b848338150000",   K_MOV,  'B', 'a'},
    {0x4aa26c, "8b848638150000",   K_MOV,  'S', 'a'}, // 通常描画
    {0x4aa377, "ffb48638150000",   K_PUSH, 'S', 0},
    {0x4aa3a3, "8b848638150000",   K_MOV,  'S', 'a'},
};

static bool apply_patches() {
    g_delta = (uintptr_t)GetModuleHandleA(nullptr) - IMAGE_BASE;
    // 事前に全箇所を検証
    bool ok = true;
    for (auto& gp : kGlyphPatches) ok &= check_bytes(gp.va, gp.bytes);
    ok &= check_bytes(0x4a8b84, "7741");
    ok &= check_bytes(0x4a8f4b, "774f");
    ok &= check_bytes(0x54e0d0, "558bec8b5508");
    ok &= check_bytes(0x4a9317, "e8a49af5ff");
    ok &= check_bytes(0x4a9815, "e8a695f5ff");
    ok &= check_bytes(0x4aa0a8, "e8138df5ff");
    ok &= check_bytes(0x4a9c7f, "8b8b5c1400008b40083bc8");
    ok &= check_bytes(0x4aa3d2, "8b865c1400003b4108");
    ok &= check_bytes(0x4a90e3, "803c38200f85a4000000");
    ok &= check_bytes(0x4ff310, "558bec6aff");
    ok &= check_bytes(0x4fcae0, "558bec538b5d1c");
    if (!ok) { logf("exe のバージョンが想定と異なるためパッチを中止しました"); return false; }

    a_strAssignSub = 0x402dc0 + g_delta;
    a_drawClipCont = 0x54e0d6 + g_delta;
    p_strAssign = (StrAssignPtr_t)(0x403150 + g_delta);

    g_stubMem = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    for (auto& gp : kGlyphPatches) {
        uint8_t pushOp = gp.base == 'S' ? 0x56 : 0x53; // push esi / push ebx
        uintptr_t stub = make_stub(gp.kind, pushOp, gp.dst);
        size_t len = gp.kind == K_CMP ? 8 : 7;
        write_call(gp.va + g_delta, stub, len);
    }
    uint8_t nop2[2] = {0x90, 0x90};
    write_mem(0x4a8b84 + g_delta, nop2, 2);
    write_mem(0x4a8f4b + g_delta, nop2, 2);
    write_call(0x54e0d0 + g_delta, (uintptr_t)&hook_drawclip, 6, 0xE9);
    write_call(0x4a9317 + g_delta, (uintptr_t)&hook_assign, 5);
    write_call(0x4a9815 + g_delta, (uintptr_t)&hook_assign, 5);
    write_call(0x4aa0a8 + g_delta, (uintptr_t)&hook_assign, 5);
    write_call(0x4a9c7f + g_delta, (uintptr_t)&hook_typeA, 11);
    write_call(0x4aa3d2 + g_delta, (uintptr_t)&hook_typeB, 9);
    p_strAssignSub = (StrAssignSub_t)(0x402dc0 + g_delta);
    a_wrapSpace = 0x4a90ed + g_delta;
    a_wrapCont = 0x4a9191 + g_delta;
    write_call(0x4a90e3 + g_delta, (uintptr_t)&hook_wrap, 10, 0xE9);
    a_opDeleteIat = 0x5b1218 + g_delta;
    write_call(0x4ff310 + g_delta, (uintptr_t)&hook_ldraw, 5, 0xE9);
    write_call(0x4fcae0 + g_delta, (uintptr_t)&hook_lwidth, 5, 0xE9);
    logf("パッチ適用完了 (delta=%08X)", (unsigned)g_delta);
    return true;
}

// ---------------------------------------------------------------- 初期化
static void init(HMODULE self) {
    wchar_t exe[1024];
    DWORD n = GetModuleFileNameW(nullptr, exe, 1024);
    if (n == 0 || n >= 1024) exe[0] = 0;
    wchar_t* sl = wcsrchr(exe, L'\\');
    if (sl) sl[1] = 0;
    swprintf_s(g_dirW, L"%sjp\\", exe);
    WideCharToMultiByte(CP_UTF8, 0, g_dirW, -1, g_dir, sizeof(g_dir), nullptr, nullptr);
    g_log = open_jp("jpfix.log", L"w");
    logf("jpfix 起動: %s", g_dir);

    load_real_version();
    init_code_tables();

    if (load_font_info()) { init_pool(); g_fontReady = true; }
    load_charmap();
    load_dict();
    g_qOpen = encode_ja("「");
    g_qClose = encode_ja("」");
    init_kinsoku();
    {
        std::vector<std::string> cfg;
        if (read_lines("config.ini", cfg)) {
            for (auto& l : cfg) {
                if (l.compare(0, 12, "test_prefix=") == 0) g_testPrefix = encode_ja(l.substr(12));
                if (l.compare(0, 18, "dump_untranslated=") == 0) g_dumpUntranslated = l[18] == '1';
            }
        }
        if (!g_testPrefix.empty()) logf("テストモード: 未翻訳文字列に接頭辞を付けます");
    }
    g_untr = open_jp("untranslated.txt", L"ab");
    apply_patches();
}

BOOL WINAPI DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        init(h);
    }
    return TRUE;
}
