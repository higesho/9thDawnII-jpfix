"""9th Dawn II 日本語フォント画像の生成

jp/strings.txt (英語<TAB>日本語) の日本語側で使われている文字と基本文字集合から、
  jp/charmap.txt   通し番号<TAB>文字
  jp/font_w.png    白文字アトラス (font19 / font22 相当)
  jp/font_d.png    暗色文字アトラス (font19brown / font22brown 相当)
  jp/font.ini      アトラスの配置情報 (DLL が読む)
を出力する。

使い方: python gen_font.py <ゲームフォルダ> [--font FONTFILE] [--preview out.png]
"""
import argparse
import os
import re
import sys
from PIL import Image, ImageDraw, ImageFont

NSLOT = 30 * 84
ATLAS = 2048

# (サイズ名, セル幅, セル高, 送り込みピッチX, ピッチY, 文字px, 原点y)
SIZES = {
    19: dict(cw=18, ch=24, px=20, py=26, fs=17, top=1),
    22: dict(cw=21, ch=26, px=23, py=28, fs=20, top=1),
}

BASE_CHARS = (
    "".join(chr(c) for c in range(0x3041, 0x3097))      # ひらがな
    + "".join(chr(c) for c in range(0x30A1, 0x30FB))    # カタカナ
    + "ー・ヴヵヶ"
    + "、。「」『』（）［］【】〈〉《》！？：；…‥～〜♪★☆●○■□◆◇→←↑↓※＋－×÷＝％＆＃＠"
    + "々〆〇"
)


def unescape_visible(s):
    # \n などのエスケープは文字集合に含めない
    return re.sub(r"\\(x[0-9A-Fa-f]{2}|.)", "", s)


def collect_chars(strings_path):
    chars = set(BASE_CHARS)
    if os.path.exists(strings_path):
        with open(strings_path, encoding="utf-8-sig") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line or line.startswith("#") or "\t" not in line:
                    continue
                ja = unescape_visible(line.split("\t", 1)[1])
                for ch in ja:
                    if ord(ch) >= 0x80:
                        chars.add(ch)
    def key(c):
        o = ord(c)
        if 0x3000 <= o < 0x3100:
            return (0, o)
        if 0x30A0 <= o < 0x3100:
            return (1, o)
        if o < 0x3000 or 0xFF00 <= o:
            return (2, o)
        return (3, o)
    return sorted(chars, key=key)


def load_font(path, size, weight):
    f = ImageFont.truetype(path, size)
    if weight:
        try:
            f.set_variation_by_axes([weight])
        except Exception:
            pass
    return f


def render_glyph(font, ch, cw, ch_h, fs, top, fg, shadow):
    """1文字をセル(cw x ch_h)に描く。em枠をセル上端 top から fs ピクセルに置く。"""
    big = Image.new("L", (cw * 2, ch_h * 2), 0)
    d = ImageDraw.Draw(big)
    # em 枠の基準: '国' の bbox から縦位置を合わせる
    ref = font.getbbox("国")
    ref_h = ref[3] - ref[1]
    oy = top + (fs - ref_h) // 2 - ref[1]
    bb = font.getbbox(ch)
    adv = font.getlength(ch)
    ox = (cw - 1 - adv) / 2 if adv <= cw else 0
    d.text((ox, oy), ch, font=font, fill=255)
    mask = big.crop((0, 0, cw, ch_h))
    cell = Image.new("RGBA", (cw, ch_h), (0, 0, 0, 0))
    if shadow:
        sh = Image.new("RGBA", (cw, ch_h), shadow[:3] + (0,))
        a = mask.point(lambda v: v * shadow[3] // 255)
        sh.putalpha(a)
        cell.alpha_composite(sh, (1, 1))
    body = Image.new("RGBA", (cw, ch_h), fg + (0,))
    body.putalpha(mask)
    cell.alpha_composite(body)
    return cell


STYLES = {
    0: dict(fg=(255, 255, 255), shadow=(30, 30, 30, 200)),   # 白
    1: dict(fg=(25, 25, 25), shadow=(150, 150, 150, 170)),   # 暗
}


def build(game_dir, font_path, weight):
    jp = os.path.join(game_dir, "jp")
    os.makedirs(jp, exist_ok=True)
    chars = collect_chars(os.path.join(jp, "strings.txt"))
    if len(chars) > NSLOT:
        sys.exit(f"文字数 {len(chars)} が上限 {NSLOT} を超えています")
    with open(os.path.join(jp, "charmap.txt"), "w", encoding="utf-8", newline="\n") as f:
        for i, c in enumerate(chars):
            f.write(f"{i}\t{c}\n")

    layout = {}
    y = 0
    for sz in (19, 22):
        s = SIZES[sz]
        cols = ATLAS // s["px"]
        rows = (len(chars) + cols - 1) // cols
        layout[sz] = dict(oy=y, cols=cols)
        y += rows * s["py"] + 8
    if y > ATLAS - 16:
        sys.exit("アトラスに収まりません")

    for t, style in STYLES.items():
        atlas = Image.new("RGBA", (ATLAS, ATLAS), (0, 0, 0, 0))
        for sz in (19, 22):
            s = SIZES[sz]
            font = load_font(font_path, s["fs"], weight)
            L = layout[sz]
            for i, c in enumerate(chars):
                x = (i % L["cols"]) * s["px"]
                yy = L["oy"] + (i // L["cols"]) * s["py"]
                atlas.alpha_composite(render_glyph(font, c, s["cw"], s["ch"], s["fs"], s["top"], style["fg"], style["shadow"]), (x, yy))
        atlas.save(os.path.join(jp, "font_w.png" if t == 0 else "font_d.png"))

    with open(os.path.join(jp, "font.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# 自動生成: {os.path.basename(font_path)} weight={weight} chars={len(chars)}\n")
        f.write(f"atlas={ATLAS}\nlead=4,{ATLAS - 4}\n")
        v = 0
        for t in (0, 1):
            for sz in (19, 22):
                s = SIZES[sz]; L = layout[sz]
                f.write(f"var{v}={t},0,{L['oy']},{s['cw']},{s['ch']},{s['px']},{s['py']},{L['cols']}\n")
                v += 1
    print(f"{len(chars)} 字を出力しました -> {jp}")


def preview(font_path, weight, out, hud_png, metrics_dir):
    """ゲームの英字グリフと日本語を並べた見本画像"""
    text_en = "Adventurer, my name is Reswick."
    text_ja = "冒険者よ、私の名はレスウィックだ。道が塞がれている！"
    hud = Image.open(hud_png)
    rows = []
    for sz, (fname, sx, sy) in {19: ("font19", 2, 1371), 22: ("font22", 272, 1100)}.items():
        t = open(os.path.join(metrics_dir, fname, "Metrics.xml"), encoding="utf-8-sig").read()
        met = {int(m[0]): tuple(map(int, m[1:])) for m in re.findall(r'key="(\d+)">\s*<x>(\d+)</x>\s*<y>(\d+)</y>\s*<width>(\d+)</width>\s*<height>(\d+)</height>', t)}
        s = SIZES[sz]
        line = Image.new("RGBA", (900, s["ch"]), (0, 0, 0, 0))
        x = 0
        for c in text_en:
            gx, gy, gw, gh = met[ord(c)]
            line.alpha_composite(hud.crop((gx + sx - 2, gy + sy - 2, gx + sx - 2 + gw, gy + sy - 2 + gh)), (x, 0))
            x += gw + 1
        font = load_font(font_path, s["fs"], weight)
        x += 10
        for c in text_ja:
            line.alpha_composite(render_glyph(font, c, s["cw"], s["ch"], s["fs"], s["top"], **STYLES[0]), (x, 0))
            x += s["cw"] + 1
        rows.append(line)
    img = Image.new("RGBA", (900, sum(r.height + 6 for r in rows) + 6), (70, 60, 50, 255))
    y = 6
    for r in rows:
        img.alpha_composite(r, (6, y)); y += r.height + 6
    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(out)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("--font", default=r"C:\Windows\Fonts\NotoSerifJP-VF.ttf")
    ap.add_argument("--weight", type=int, default=600)
    ap.add_argument("--preview")
    ap.add_argument("--hud")
    a = ap.parse_args()
    if a.preview:
        preview(a.font, a.weight, a.preview, a.hud, os.path.join(a.game_dir, "data", "font"))
    else:
        build(a.game_dir, a.font, a.weight)
