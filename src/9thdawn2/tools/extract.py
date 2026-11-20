"""ゲームデータから翻訳対象の英語文字列を抽出する。

出力: jp_mod/trans/src/<カテゴリ>.txt
  各行  ID<TAB>英語(エスケープ済み)
  '## ' で始まる行は文脈用の見出し(会話のNPC名など)
エスケープ: \\n 改行, \\xHH 非ASCIIバイト, \\\\ バックスラッシュ

使い方: python extract.py <ゲームフォルダ>
"""
import glob
import os
import re
import sys

SPLIT = re.compile(rb"\xe8b|[\xef\xd1\xe8\xf4\xc6\xe9\r\n]")
COLOR = re.compile(rb"\xab(?:-?\d+\xef){5}")
SKIP_EXACT = {b"<DELETED>", b"<EmptyMessage>", b"Entry Node->", b"NO_TITLE", b"Continent"}


def esc(b):
    out = []
    for c in b:
        if c == 0x0A:
            out.append("\\n")
        elif c == 0x5C:
            out.append("\\\\")
        elif c < 0x20 or c >= 0x80:
            out.append("\\x%02X" % c)
        else:
            out.append(chr(c))
    return "".join(out)


def is_text(f):
    if not re.search(rb"[A-Za-z]{2}", f):
        return False
    if f in SKIP_EXACT or f.startswith(b"_") or f.startswith(b"NNN"):
        return False
    s = f.strip()
    if re.fullmatch(rb"N+n?\d*", s):
        return False
    # 空白を含まない識別子っぽいもの(数字混在・キャメルケース・アンダースコア)は除外
    if b" " not in s:
        if re.search(rb"[_\\/]|\d", s) and not re.fullmatch(rb"[A-Z][a-z]+\d?", s):
            return False
        if re.search(rb"[a-z][A-Z]", s):
            return False
    return True


def fields(data):
    """色コードを保護しつつ区切りで分割し、0xC5 を改行に変換したフィールドを返す"""
    keep = {}

    def protect(m):
        k = b"\x01%d\x02" % len(keep)
        keep[k] = m.group(0)
        return k

    data = COLOR.sub(protect, data)
    for raw in SPLIT.split(data):
        f = raw.replace(b"\xc5", b"\n")
        for k, v in keep.items():
            f = f.replace(k, v)
        if f.startswith(b"Nn!"):
            f = f[3:]
        yield f


def write(path, rows):
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        for r in rows:
            fp.write(r + "\n")


def main(game):
    root = os.path.join(game, "data", "_g_9thDawnII")
    out = os.path.join(game, "jp_mod", "trans", "src")
    os.makedirs(out, exist_ok=True)
    seen = set()
    counts = {}

    def take(cat, prefix, items):
        rows = []
        n = 0
        for it in items:
            if isinstance(it, str):  # 見出し
                rows.append(it)
                continue
            if it in seen:
                continue
            seen.add(it)
            n += 1
            rows.append(f"{prefix}{n:04d}\t{esc(it)}")
        # 中身のない見出しを除く
        cleaned = []
        for i, r in enumerate(rows):
            if r.startswith("## ") and (i + 1 >= len(rows) or rows[i + 1].startswith("## ")):
                continue
            cleaned.append(r)
        write(os.path.join(out, cat + ".txt"), cleaned)
        counts[cat] = n

    # 会話: 'èb' で始まらない行の名前を見出しにする
    def dialogue():
        d = open(os.path.join(root, "editor", "dialoguedatabase", "data.txt"), "rb").read()
        for line in d.split(b"\r\n"):
            if not line.startswith(b"\xe8b"):
                names = [f for f in fields(line) if is_text(f)]
                if names:
                    yield "## " + esc(names[0].replace(b"<DELETED> ", b""))
                    for n in names[1:]:
                        yield n
                continue
            for f in fields(line):
                if is_text(f):
                    yield f

    def db(*names):
        for name in names:
            for p in sorted(glob.glob(os.path.join(root, name))):
                d = open(p, "rb").read()
                yield "## " + os.path.relpath(p, root).replace("\\", "/")
                for f in fields(d):
                    if is_text(f):
                        yield f

    take("dialogue", "d", dialogue())
    take("items", "i", db("editor/itemdatabase/data.txt"))
    take("quests", "q", db("editor/quests/data.txt", "editor/literature/data.txt", "editor/rewards/data.txt"))
    take("world", "w", db("editor/spells/data.txt", "editor/shops/data.txt", "editor/switches/data.txt",
                          "editor/events/data.txt", "editor/factions/data.txt", "editor/entitydatabase/data.txt",
                          "editor/mapobjectdatabase/data.txt", "maps/*_entities.txt", "maps/*_mobjects.txt",
                          "gui/desktop/windows/*.txt", "editor/terraindatabase/data.txt", "editor/traps/data.txt"))
    # exe 内の文字列 (.rdata)。エディタ用も多く含むので翻訳時に選別する
    def exe():
        import pefile
        pe = pefile.PE(os.path.join(game, "ninthdawnii.exe"))
        rd = [x for x in pe.sections if x.Name.startswith(b".rdata")][0].get_data()
        for m in re.finditer(rb"(?<=\x00)[\x20-\x7e\n]{2,}(?=\x00)", rd):
            t = m.group(0)
            if not re.search(rb"[A-Za-z]{2}", t) or re.search(rb"^[?@.]|@@|\.(txt|png|xml|dat|wav|dll)$|^[A-Z_0-9]+$|\\", t):
                continue
            if b" " not in t and not re.fullmatch(rb"[A-Z][a-z]+:?", t):
                continue
            yield t
    take("ui", "u", exe())
    # 熟練度名・能力値の説明・状態効果名 (既存IDを変えないよう最後に追加)
    take("build", "b", db("editor/builddatabase/data.txt"))
    for k, v in counts.items():
        print(f"{k}: {v} 件")


if __name__ == "__main__":
    main(sys.argv[1])
