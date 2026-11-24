# 9th Dawn II 日本語化 version.dll のソース

Steam 版 9th Dawn II の文字表示を日本語にする `version.dll` と、その組み立てに使う道具です。

- `src/` … version.dll 本体（C++）。Visual Studio 2022 の x86 ツールで `build.bat` を実行すると `build\version.dll` ができます。`build.bat test` とすると、試験用の機能（`config.ini` の `test_prefix=`）を入れた試験版が `build\test\version.dll` にできます。試験版は配布しません。
- `tools/` … Python の道具。
  - `extract.py <ゲームフォルダ>` … 原作の文章を集めます。
  - `merge.py <ゲームフォルダ>` … 訳をまとめて `jp\strings.txt` を作り、`gen_font.py` で文字の画像を作ります。
  - `make_release.py <ゲームフォルダ> <版>` … 配布用の zip を作ります。

必要なもの: Python 3（Pillow）、Noto Serif JP、訳の表（このリポジトリには入っていません）。
ゲーム本体は各自で用意してください。
