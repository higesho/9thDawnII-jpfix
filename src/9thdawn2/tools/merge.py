"""翻訳結果を統合して jp/strings.txt を作り、フォント画像も再生成する。

入力:
  jp_mod/trans/src/*.txt     ID<TAB>英語 (extract.py の出力)
  jp_mod/trans/out/*.txt     ID<TAB>日本語 (翻訳結果)
  jp_mod/trans/manual.txt    英語<TAB>日本語 (手動辞書・パターン。最優先)
出力:
  jp/strings.txt
検証:
  色指定コード・改行数の不一致、全角英数字、未翻訳ID などを警告として表示する。

使い方: python merge.py <ゲームフォルダ> [--no-font]
"""
import glob
import os
import re
import subprocess
import sys

COLOR = re.compile(r"\\xAB(?:-?\d+\\xEF){5}")


def read_tsv(path):
    rows = []
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#") or line.startswith("## ") or "\t" not in line:
                continue
            a, b = line.split("\t", 1)
            rows.append((a, b))
    return rows


def main(game, make_font=True):
    trans = os.path.join(game, "jp_mod", "trans")
    src = {}
    for p in glob.glob(os.path.join(trans, "src", "*.txt")):
        for i, en in read_tsv(p):
            src[i] = en
    warn = []
    out = {}
    for p in sorted(glob.glob(os.path.join(trans, "out", "*.txt"))):
        for i, ja in read_tsv(p):
            i = i.strip()
            if i not in src:
                warn.append(f"{os.path.basename(p)}: 不明なID {i}")
                continue
            ja = ja.strip("\t")
            en = src[i]
            # 先頭・末尾の空白は原文に合わせる (連結される断片の区切りを保つため)
            lead = en[: len(en) - len(en.lstrip(" "))]
            trail = en[len(en.rstrip(" ")):]
            ja = lead + ja.strip(" ") + trail
            if sorted(COLOR.findall(en)) != sorted(COLOR.findall(ja)):
                warn.append(f"{i}: 色コード不一致 | {en[:60]} | {ja[:60]}")
            if en.count("\\n") != ja.count("\\n"):
                warn.append(f"{i}: 改行数が異なる ({en.count(chr(92)+'n')}→{ja.count(chr(92)+'n')})")
            if re.search(r"[Ａ-Ｚａ-ｚ０-９]", ja):
                warn.append(f"{i}: 全角英数字 | {ja[:60]}")
            if "\t" in ja:
                warn.append(f"{i}: タブを含む")
            out[i] = ja
    dict_ = {}
    for i, ja in out.items():
        dict_[src[i]] = ja
        # エンティティ名の前置記号 n! は表示時に外れている可能性があるので両方登録
        if src[i].startswith("n!") and ja.startswith("n!"):
            dict_.setdefault(src[i][2:], ja[2:])
        # 書物ページ先頭の制御文字 N は表示時に外れるので、外した形も登録
        m = re.match(r"N+(?=[A-Z\\])", src[i])
        if m:
            dict_.setdefault(src[i][m.end():], re.sub(r"^N+", "", ja))
    manual = os.path.join(trans, "manual.txt")
    n_manual = 0
    if os.path.exists(manual):
        for en, ja in read_tsv(manual):
            dict_[en] = ja
            n_manual += 1
    # 未翻訳ID (ui系はスキップ可なので除外して数える)
    missing = [i for i in src if i not in out and not i.startswith("u")]
    with open(os.path.join(game, "jp", "strings.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# 自動生成 (merge.py)。手動の追加・修正は jp_mod/trans/manual.txt へ\n")
        for en, ja in dict_.items():
            f.write(f"{en}\t{ja}\n")
    chars = set()
    for ja in dict_.values():
        for ch in re.sub(r"\\(x[0-9A-Fa-f]{2}|.)", "", ja):
            if ord(ch) >= 0x80:
                chars.add(ch)
    print(f"辞書 {len(dict_)} 件 (翻訳 {len(out)} / 手動 {n_manual})、使用文字 {len(chars)} 種")
    print(f"未翻訳ID(ui以外): {len(missing)}" + (f" 例: {missing[:10]}" if missing else ""))
    print(f"警告 {len(warn)} 件")
    for w in warn[:80]:
        print("  " + w)
    if make_font:
        subprocess.check_call([sys.executable, "-I", "-X", "utf8", os.path.join(os.path.dirname(__file__), "gen_font.py"), game])


if __name__ == "__main__":
    main(sys.argv[1], "--no-font" not in sys.argv)
