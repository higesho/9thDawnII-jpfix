"""配布パッケージを作る。

  jp/strings.txt (英語キー) から、英語原文を含まない配布用辞書 (キー = 原文の FNV-1a 64bit) を作り、
  version.dll・フォント・説明書・ライセンスと一緒に jp_mod/dist/ に zip でまとめる。
  {n} / {tn} を含むパターン行は照合に原文の断片が必要なので英語のまま残す。

使い方: python make_release.py <ゲームフォルダ> <バージョン> [--contact 連絡先]
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile

PATTERN = re.compile(r"\{t?\d\}")


def unescape(s):
    """DLL の unescape と同じ規則でバイト列にする"""
    out = bytearray()
    i = 0
    b = s.encode("utf-8")
    while i < len(b):
        c = b[i]
        if c == 0x5C and i + 1 < len(b):
            n = chr(b[i + 1])
            if n == "n": out.append(0x0A); i += 2; continue
            if n == "t": out.append(0x09); i += 2; continue
            if n == "r": out.append(0x0D); i += 2; continue
            if n == "\\": out.append(0x5C); i += 2; continue
            if n == "x" and i + 3 < len(b):
                out.append(int(b[i + 2:i + 4].decode(), 16)); i += 4; continue
            out += b[i:i + 2]; i += 2; continue
        out.append(c); i += 1
    return bytes(out)


def fnv1a64(data):
    h = 1469598103934665603
    for c in data:
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("version")
    ap.add_argument("--contact", default="（未記入）")
    a = ap.parse_args()
    game = a.game
    mod = os.path.join(game, "jp_mod")
    jp = os.path.join(game, "jp")
    name = f"9thDawnII_JP_v{a.version}"
    out = os.path.join(mod, "dist", name)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "jp"))
    os.makedirs(os.path.join(out, "licenses"))

    # 辞書のハッシュ化
    rows, seen = [], {}
    n_hash = n_pat = 0
    with open(os.path.join(jp, "strings.txt"), encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#") or "\t" not in line:
                continue
            en, ja = line.split("\t", 1)
            if PATTERN.search(en):
                rows.append(f"{en}\t{ja}")
                n_pat += 1
                continue
            h = fnv1a64(unescape(en))
            if h in seen and seen[h] != en:
                sys.exit(f"ハッシュ衝突: {en!r} / {seen[h]!r}")
            seen[h] = en
            rows.append(f"@{h:016X}\t{ja}")
            n_hash += 1
    with open(os.path.join(out, "jp", "strings.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# 9th Dawn II 日本語化パッチ v{a.version} 翻訳辞書 (キーは原文の FNV-1a 64bit)\n")
        f.write("\n".join(rows) + "\n")

    for fn in ("charmap.txt", "font_w.png", "font_d.png", "font.ini"):
        shutil.copy2(os.path.join(jp, fn), os.path.join(out, "jp", fn))
    with open(os.path.join(out, "jp", "config.ini"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("# 9th Dawn II 日本語化パッチ 設定\n"
                "# 1 にすると、訳がなかった文を jp\\untranslated.txt に記録する\n"
                "dump_untranslated=0\n")
    shutil.copy2(os.path.join(mod, "build", "version.dll"), os.path.join(out, "version.dll"))

    # フォントのライセンス (OFL 1.1 本文 + Noto Serif JP の著作権表記)
    ofl = open(os.path.join(mod, "release", "OFL-1.1.txt"), encoding="utf-8").read()
    with open(os.path.join(out, "licenses", "OFL-NotoSerifJP.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("Noto Serif JP\nCopyright 2017-2023 Adobe (http://www.adobe.com/).\n\n"
                "This Font Software is licensed under the SIL Open Font License, Version 1.1.\n"
                "The glyph images in jp\\font_w.png and jp\\font_d.png were rendered from this font.\n\n")
        f.write(ofl)

    # 説明書 (メモ帳で文字化けしないよう BOM 付き UTF-8 / CRLF)
    exe = os.path.join(game, "ninthdawnii.exe")
    sha = hashlib.sha256(open(exe, "rb").read()).hexdigest()
    acf = os.path.join(game, "..", "..", "appmanifest_279070.acf")
    m = re.search(r'"buildid"\s+"(\d+)"', open(acf, encoding="utf-8").read()) if os.path.exists(acf) else None
    readme = open(os.path.join(mod, "release", "お読みください.txt"), encoding="utf-8").read()
    readme = readme.replace("{VERSION}", a.version).replace("{EXESHA}", sha) \
                   .replace("{BUILDID}", m.group(1) if m else "不明").replace("{CONTACT}", a.contact)
    with open(os.path.join(out, "お読みください.txt"), "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write(readme)

    # zip
    zpath = os.path.join(mod, "dist", name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(out):
            for fn in files:
                p = os.path.join(root, fn)
                # zip 直下に version.dll と jp を置く (フォルダごとコピーして一段深くなる誤りを防ぐ)
                z.write(p, os.path.relpath(p, out))
    print(f"辞書: ハッシュ {n_hash} 件 / パターン {n_pat} 件")
    print(f"出力: {zpath} ({os.path.getsize(zpath) // 1024} KB)")


if __name__ == "__main__":
    main()
