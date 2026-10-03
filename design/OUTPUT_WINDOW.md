# OUTPUT WINDOW — 映像は拡張スクリーンに全画面、設定画面は手元の別窓

2026-10-03 / branch `feature/output-window`(`feature/crowd` から分岐)

## やりたいこと

- プロジェクター(拡張スクリーン)には **映像 + CROWD ゲージだけ** を枠なし全画面で出す
- Controls(メニュー・エフェクト設定)は **手元のノートPCの画面に独立した OS ウィンドウ** として出す
- 今は 1 枚の GLFW 窓に映像と ImGui を重ねており、F11 は常にプライマリモニターに出る

## 方式: ImGui を docking 版にしてマルチビューポートを使う

- `CMakeLists.txt` の ImGui を `v1.91.0` → `v1.91.0-docking`(同じ版の docking ブランチ。API 差は最小)
- `io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable`
- `io.ConfigViewportsNoAutoMerge = true` … Controls が出力窓に吸い込まれない(常に別 OS 窓)
- メインの GLFW 窓 = **出力窓**(ImGui ウィンドウは置かない。ゲージだけ)
- フレーム末尾で `UpdatePlatformWindows()` / `RenderPlatformWindowsDefault()`、その後コンテキストを出力窓へ戻して swap
- 副窓は backend が共有コンテキストで作り swapInterval(0)。vsync は出力窓だけ

代案(不採用): 2 つ目の GLFW 窓を自前で作り、FBO に描いた映像を共有テクスチャで blit する。
ゲージ(ImGui の前景 draw list)を出力側にも描く手段が要り、コード量が倍になる。

## 出力窓の全画面

- 起動時にモニターを列挙。**プライマリ以外があれば、それを出力先にして自動で枠なし全画面**
  - `--windowed` で自動全画面を抑止、`--output-monitor N` で番号指定
- 枠なし全画面 = `GLFW_DECORATED=false` + 位置/サイズをそのモニターの `glfwGetMonitorPos` + vidmode に合わせる
  - 排他フルスクリーン(`glfwSetWindowMonitor(win, mon, ...)`)は使わない。
    フォーカスが Controls 側へ移ると AUTO_ICONIFY で最小化されるため
- Controls に「Output」欄: モニター選択コンボ / Fullscreen チェック
- F11 = 選択中モニターで全画面⇔窓の切替
- `glfwSetMonitorCallback`: 出力先モニターが抜けたら窓モードに戻してプライマリへ退避
- 全画面中に出力窓の上にマウスがある時はカーソルを消す
  (backend が毎フレーム カーソルを上書きするので、`glfwGetWindowAttrib(window, GLFW_HOVERED)` のとき
   `ImGui::SetMouseCursor(ImGuiMouseCursor_None)` で ImGui 経由で消す)

## Controls 窓の位置

- `SetNextWindowPos(..., ImGuiCond_FirstUseEver)` で **プライマリモニターの作業領域の左上 + 余白**、サイズ 560x820
- 位置は imgui.ini に残る(デスクトップ絶対座標)
- 会場でモニター配置が変わって画面外に行った時の救済: **F2 = Controls をプライマリモニターへ呼び戻す**

## ホットキー

- 今は `glfwGetKey(window, ...)` = 出力窓にフォーカスがある時しか効かない。
  Controls 窓を触った直後は効かなくなる
- → `ImGui::IsKeyPressed(ImGuiKey_F11, false)` / `IsKeyDown(ImGuiKey_T)` に置換。
  backend は全ビューポート窓のキーを io に流すので、どちらの窓が前面でも効く
- F1 = Controls を隠す/出す(従来どおり)。F2 = 呼び戻し。T/B/R は従来どおり WantCaptureKeyboard 中は無視

## ゲージの座標(要注意)

- ビューポート有効時、ImGui の座標は **デスクトップ絶対座標**。
  今のゲージは `io.DisplaySize` 基準・原点 (0,0) で描いているので、拡張スクリーン上ではずれる
- → `ImGuiViewport* mv = ImGui::GetMainViewport()` の `Pos` / `Size` 基準に直し、
  `GetForegroundDrawList(mv)` で出力窓に確実に描く

## 検証設計

1. 成功条件: 本人が拡張スクリーンに映像だけ、手元に Controls を見て、操作して違和感がないこと(判断者=本人の目)
2. 間違うと痛い主張
   - (a) ゲージが拡張スクリーン上の正しい位置に出る(座標系の取り違え)
   - (b) Controls を触った後でもホットキーが効く
   - (c) 本番中にモニターが抜けても落ちない・映像窓が迷子にならない
3. 潰し方(生成経路と別)
   - CI でビルドが通る(コンパイル)
   - このPC(モニター1枚)で CI の exe を実際に起動し、スクショで「出力窓と Controls 窓が別窓」「ゲージが出力窓の下端」を目視
     (Keyboard test mode で T 長押し → ゲージ)
   - (a)(c) の 2 枚構成は本人の PC + プロジェクターでしか確認できない → チェックリストを渡す
4. 検証できない部分: 2 枚目のモニター上の見え方・抜き差し。報告に未検証として明記

## GPT レビューで変えた点(2026-10-03)

設計レビュー:
- カーソルは backend 後に出力窓だけ上書き案 → 採らず、`SetMouseCursor(None)` を「出力窓が HOVERED の時だけ」。
  既知の妥協: ループが止まった瞬間にポインタが Controls へ移ると、次フレームまで Controls 上でも消える。Controls に切替チェックあり
- ImGui backend は自前の monitor callback を張り、後から張ったものをチェインしない → アプリの callback から
  `ImGui_ImplGlfw_MonitorCallback` を先に呼ぶ
- Controls は `Begin("Controls", &showUI)`、サイズは work area に収める、F2 は `ImGuiCond_Always`、imgui.ini は exe 横に固定
- 本番用: BLACK(F12)、Identify、フレーム時間の最悪値

完成レビュー:
- BLACK 中は Identify も出さない。Identify の自動表示は起動時だけ(本番中の再全画面化で客席に番号を出さない)
- 「操作画面 = primary」を前提にしない: Controls の初期位置 / F2 は「出力ではない画面」。全画面化した画面に Controls が乗っていたら退避
- `--output-monitor N` が存在しない番号なら窓のまま(別画面へ勝手に出さない)
- 窓の変更(combo / F11)は次フレーム頭で適用(そのフレームの viewport 矩形と framebuffer を食い違わせない)
- モニター列挙が一瞬 0 件 → 前の一覧を保持して次フレーム再試行。vidmode の無いモニターは候補にしない
- 抜けた時に戻す窓サイズを primary の work area に収める。combo の選択は番号でなく画面で追う
- `--black`、BLACK 中は Controls に赤い帯
