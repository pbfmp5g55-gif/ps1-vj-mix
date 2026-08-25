# 引き継ぎメモ(日本語) — PS1エミュVJ 一式

最終更新: 2026-08-25(この日に観客スマホ機能を実装)/ その前は 2026-05-15

このファイルは「久しぶりに開いた自分」向け。英語の README / USAGE は
仕様と使い方、こっちは**現在地と次の一手**。

---

## 1. これは何か

PS1エミュ(PCSX-Redux)が**ポリゴンを描く直前の命令そのもの**を横取りして
壊す VJ システム。最終画面へのポストエフェクトではないので、キャラ・背景・
UI が個別にバラける。2台のエミュを走らせて混ぜられる。

リポジトリは3本。役割が違うので、直す場所を間違えないこと。

| リポジトリ | 中身 | 最新版 | ライセンス |
|---|---|---|---|
| `ps1-primitive-vj` | **libvj**。グリッチ本体(頂点ずらし/UVずらし/間引き/色/描画順遅延)、MIDI、フィルタプリセット、ストリーム形式 | main のみ(タグ無し) | MIT |
| `pcsx-redux`(フォーク) | エミュ本体。`vj-integration` ブランチ。命令を横取りして共有メモリへ送る側 | **v0.7.10** | GPL v2 |
| `ps1-vj-mix` | **ミキサー**。2チャンネル受けて混ぜて描く。実演で触るのはこれ | **v0.10.2** | MIT |

`ps1-vj-mix` は libvj を `third_party/libvj/` に **git submodule** で抱えている。
クローンは必ず `--recurse-submodules` を付ける。

ローカルの置き場: `C:\Users\yuho_shinkawa\Desktop\claude_repo\ps1-vj-mix` と
`...\ps1-primitive-vj`(2026-08-25 に取得)。**pcsx-redux フォークはローカルに無い**
— 巨大なので、エミュ側を直す用事ができてから `gh repo clone` する。

## 2. どこまで出来ているか

- **Phase A / B / C 完了**。1台でも2台でも動く。クロスフェード、VRAM を左右に
  分ける「きれいな2画面」、その中間の「ぶつかり具合」つまみまで入っている
- CLUT(パレット絵)の表示モード6種、PS1の半透明4種、4x4ディザ、スプライト横取り
- グリッチ8種(MASTER/CHANCE/GEOMETRY/TEXTURE/MISSING/COLOR/DEPTH/CHAOS)
- AutoMode(LFOで勝手に揺らす)、フィルタプリセットバンク(範囲・面積・テクスチャ有無で
  対象を絞る。ファイル保存あり)
- `.vjr` 録画の再生、Twin Self(過去フレームの残像) ※**Twin Self はファイル再生時のみ**
- MIDI入力(RtMidi)、CC の Learn
- `-vjring` / `--attach-a` で自動接続 → bat 一発で起動できる

未マージのブランチ無し、Issue 無し。**きれいに止まっている**。

## 3. 再開するときの手順(要約)

詳細は `USAGE.md`。最短は:

1. GitHub Releases から `pcsx-redux` フォーク **v0.7.10** と `ps1-vj-mix` **v0.10.2** の ZIP を落とす
2. `openbios.bin`(524288 バイト、MIT)を `pcsx-redux.exe` の隣に置く
3. `pcsx-redux.exe -bios openbios.bin -iso <game.cue> -vjring Local\vj-mix-prim-A -run`
4. `vj-mix-spike1.exe --attach-a Local\vj-mix-prim-A`

映らない時の切り分け: `vj-mix-ipc-selftest.exe` を先に走らせる。
ALL OK ならミキサー側は正常 → エミュ側(Start live 押したか、ポーズしてないか)を疑う。

## 4. 踏んだ罠(次も踏むやつ)

- **Clean CLUT が真っ黒** → フォークが古い。v0.7.0 は `hostTag=0` を送るので
  パレット参照が VRAM(0,0) を見にいく
- **チラつく / `dropped=` が増える** → IPC リングが溢れている。フォーク v0.7.10 で
  32MB になっている。スプライトの多い場面は 8MB では足りない
- **Phase C で絵が壊れる** → CLUT モードは *Clean CLUT (inline palette)* を使う。
  VRAM を移動させると VRAM 参照版のパレット読みは付いてこない
- **グリッチ8種は MIDI で回せない**。CC が割り当ててあるのは Twin Self 3種 /
  CLUT モード / クロスフェード / VRAM relocate の6つだけ。8種は GUI スライダーのみ
- `git clone` で submodule を忘れるとビルドが通らない

## 5. 観客がスマホからエフェクトをかける(CROWD)

**2026-08-25 に実装した。ブランチ `feature/crowd`。まだ main には入れていない。**

つまみを奪い合わせるのではなく、**全員の連打を足し算してゲージにする**方式。
溜まり具合が常時じわじわ効き、満タンで一発ドカンと来る。

- 設計と経緯: [`design/CROWD_CONTROL.md`](design/CROWD_CONTROL.md)
- サーバ側の使い方: [`crowd-server/README.md`](crowd-server/README.md)

### 今どこまで出来ているか

- **サーバ側(Node)は手元でテスト済み。**ゲージ計算19項目 + 実プロセス起動での
  通信テスト19項目 + 12分の連続稼働。npm install は要らない
- **ミキサー側(C++)はコンパイルが通っただけ。**このPCに C++ のビルド環境が無いので、
  GitHub Actions でビルドしている。**画面に出したところは誰も見ていない**

### ★次にやること

**まず画面を見る。**サーバもスマホも要らない:

1. GitHub Actions の `feature/crowd` の成果物から `vj-mix-spike1.exe` を落とす
2. 起動して Controls → `CROWD (audience phones)` にチェック →
   `Keyboard test mode` にもチェック
3. **T を押し続けると溜まる / B で一発 / R でリセット**
4. GEOMETRY と COLOR だけで気持ちいいか、一発の強さと長さが合っているかを決める

ここが駄目なら通信は関係ないので、先にここ。良ければサーバを起動して
スマホを繋ぐ(`start-crowd.bat`)。

### 実機で確かめていないこと

スマホ実機(iOS/Android)、会場Wi-Fi、本当の人数、プロジェクタ上でのゲージの見え方。

## 6. 関連する別リポジトリ

- `pdg-manga-collage` — libvj のエフェクトを流用した漫画コラージュ生成
- `ps1-acid-rom` — PS1実機で走るシーケンサ。VJ とは別系統だが同じ PS1 遊び
