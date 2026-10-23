# sgnano_remote

sgnano の UART の遠隔コマンド（`@SG` で答える行、sgnano の `src/input/sgremote.c`・`src/filer/sgremote_work.c`）を使う、PC 側の 2 画面のファイル管理（Qt 6）。
左が PC、右が sgnano。転送は PC 側の内蔵 ZMODEM（`src/zmodem.cpp`、sgnano の `src/filer/zmodem.c` と同じ決まり）で行い、
外部の実行ファイル（`scripts/zm.py`・lrzsz・python）は使わない。

## できること
- 接続（シリアルポートとボーレート、既定 `/dev/ttyACM0` 921600）、ドライブの一覧、フォルダの一覧（名前・大きさ・更新日時・属性）、移動
- **S:（内蔵フラッシュ、2 MB、FAT12）** に対応（2026-10-24。sgnano の `src/storage/sysdisk.c`）。
  本体は USB のドライブを A から順に S を飛ばす。S: のルートの `SystemSettings`・`SystemFiles`・`SystemLogs` は
  システムのフォルダで、消す・名前を変える操作は送らずに断る（本体も `EACCES` で断る）
- アップロード・ダウンロード（ボタン、ドラッグ・アンド・ドロップ）。PC 側の内蔵 ZMODEM で送る・受け取る
  （2026-10-24。`ZPUT` / `ZGET` のあと、sgnano の `zm_receive` / `zm_send` と話す。外部の実行ファイルは要らない）。
  受け取りは CRC16 で送り、16 進・2 進（CRC32 も）の見出しを読む。本体の都合で 1 ファイル 4 MB、名前は 131 バイトまで。
  送る前に確かめ、断られたファイルはあるとまとめて知らせる
- **新しいフォルダ・名前の変更（F2）・削除（Delete、確かめる。複数可。フォルダは空のときだけ）**（2026-10-23。sgnano の `MKDIR` / `REN` / `DEL`）。
  ボタンと右クリックのメニュー。終わったら一覧を読み直し、できなかった操作はまとめて知らせる。
  多数の削除は `RemoteDeleteQueue` で 8 件ずつ順に送る（`RemoteProtocol` の待ち行列をあふれさせない）。
  削除などの操作中は、移動・更新・パス入力を受け付けない

## ビルド
```
cmake -B build -G Ninja && ninja -C build
```

## 試し（2026-10-24）
- `ctest --test-dir build --output-on-failure`: 実機なしの試しを CTest から実行する
- `build/sgnano_remote_selftest`: 実機なし。`RemoteProtocol` に答えを流し込み、DEL / REN / MKDIR の送る行（REN はタブ区切り）、OK / ERR の受け取り、
  断る形（空・制御文字・待ち行列がいっぱい）、待っていない OK を受け流すこと、並べた要求を 1 つずつ送ることを確かめる。
  `RemoteDeleteQueue` の 8 件ずつの送信、34 件以上の完走、途中失敗、タイムアウト、同期的な返事も確かめる。
  S: の見分け（`RemoteFs::isSystemDrivePath`）と、システムのフォルダの守り（`RemoteFs::isProtectedSystemPath`）、`DRIVES` の S: の受け取りも確かめる
- `build/sgnano_remote_zmodem_selftest`: 実機なし。内蔵 ZMODEM の送る側と受け取る側を socketpair で向かい合わせ、
  複数ファイル・300 KB・空のファイル・逃がす文字（ZDLE / XON / XOFF / CR / 0x7F / 0xFF）の中身と名前・数を確かめる
- `build/sgnano_remote_selftest --device /dev/ttyACM0`: 実機あり。`DRIVES` の種類で **HDD の exFAT（3）と NTFS（4）だけ**を選び（ドライブ文字はつなぐ順で変わる）、
  作る・一覧で確かめる・空白を含む名前に変える・中身のあるフォルダの削除を断られる・消す・残っていないことを通す。作ったもの（`!qt_selftest`）は消してから終わる。
  S:（内蔵フラッシュ）があることと、そのルートの 3 つのシステムのフォルダがシステムの属性で並ぶことも見る（消す・名前を変える試しはしない）
- `build/sgnano_remote_selftest --device-many /dev/ttyACM0`: 実機あり。同じく exFAT / NTFS に 34 個のフォルダを作り、`RemoteDeleteQueue` で 8 件ずつ消して、
  全部残らず消えたことを確かめる。作ったもの（`!qt_selftest_many`）は消してから終わる
