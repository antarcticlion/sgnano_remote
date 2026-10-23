# sgnano_remote

sgnano の UART の遠隔コマンド（`@SG` で答える行、sgnano の `src/input/sgremote.c`・`src/filer/sgremote_work.c`）を使う、PC 側の 2 画面のファイル管理（Qt 6）。
左が PC、右が sgnano。転送は sgnano の `scripts/zm.py`（ZMODEM、lrzsz）を呼ぶ（`SGNANO_ZM` で場所を指定できる）。

## できること
- 接続（シリアルポートとボーレート、既定 `/dev/ttyACM0` 921600）、ドライブの一覧、フォルダの一覧（名前・大きさ・更新日時・属性）、移動
- アップロード・ダウンロード（ボタン、ドラッグ・アンド・ドロップ）
- **新しいフォルダ・名前の変更（F2）・削除（Delete、確かめる。複数可。フォルダは空のときだけ）**（2026-10-23。sgnano の `MKDIR` / `REN` / `DEL`）。
  ボタンと右クリックのメニュー。終わったら一覧を読み直し、できなかった操作はまとめて知らせる。
  多数の削除は 8 件ずつ順に送る（`RemoteProtocol` の待ち行列をあふれさせない）

## ビルド
```
cmake -B build -G Ninja && ninja -C build
```

## 試し（2026-10-23）
- `build/sgnano_remote_selftest`: 実機なし。`RemoteProtocol` に答えを流し込み、DEL / REN / MKDIR の送る行（REN はタブ区切り）、OK / ERR の受け取り、
  断る形（空・制御文字）、待っていない OK を受け流すこと、並べた要求を 1 つずつ送ることを確かめる
- `build/sgnano_remote_selftest --device /dev/ttyACM0`: 実機あり。`DRIVES` の種類で **HDD の exFAT（3）と NTFS（4）だけ**を選び（ドライブ文字はつなぐ順で変わる）、
  作る・一覧で確かめる・空白を含む名前に変える・中身のあるフォルダの削除を断られる・消す・残っていないことを通す。作ったもの（`!qt_selftest`）は消してから終わる
