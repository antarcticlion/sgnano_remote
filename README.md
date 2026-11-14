# sgnano_remote

sgnano の UART の遠隔コマンド（`@SG` で答える行、sgnano の `src/input/sgremote.c`・`src/filer/sgremote_work.c`）を使う、PC 側の 2 画面のファイル管理（Qt 6）。
左が PC、右が sgnano。転送は PC 側の内蔵 ZMODEM（`src/zmodem.cpp`、sgnano の `src/filer/zmodem.c` と同じ決まり）で行い、
外部の実行ファイル（`scripts/zm.py`・lrzsz・python）は使わない。

## できること
- 接続（シリアルポートとボーレート、既定 `/dev/ttyACM0` 921600）、ドライブの一覧、フォルダの一覧（名前・大きさ・更新日時・属性）、移動
- **S:（内蔵フラッシュ、4 MB、FAT12）** に対応（2026-10-24、2026-11-02 に 2 MB から 4 MB へ。sgnano の `src/storage/sysdisk.c`）。
  本体は USB のドライブを A から順に S を飛ばす。S: のルートの `SystemSettings`・`SystemFiles`・`SystemLogs` は
  システムのフォルダで、消す・名前を変える操作は送らずに断る（本体も `EACCES` で断る）。
  2026-11-02 に増えたルートの `bin`・`sbin` は本体が守らないので、このアプリも守らない
- アップロード・ダウンロード（ボタン、ドラッグ・アンド・ドロップ）。PC 側の内蔵 ZMODEM で送る・受け取る
  （2026-10-24。`ZPUT` / `ZGET` のあと、sgnano の `zm_receive` / `zm_send` と話す。外部の実行ファイルは要らない）。
  受け取りは CRC16 で送り、16 進・2 進（CRC32 も）の見出しを読む。名前は 131 バイトまで。
  **4 MB を越えるダウンロード**は、本体が送りながら機器から読む（2026-10-29）ので、そのまま受け取れる。
  **4 MB を越えるアップロード**は、PC で `name.001 …` に分けて送り、本体の `JOIN` でつないでから分割ファイルを消す
  （2026-11-14。既定は 64 MB 以下なら 1 つ 1 MB、64 MB を越えるものは 2 MB。`SGNANO_ZM_PART` で 512 の倍数に固定できる。本体の JOIN は 64 個まで）
- **新しいフォルダ・名前の変更（F2）・削除（Delete、確かめる。複数可。フォルダは空のときだけ）**（2026-10-23。sgnano の `MKDIR` / `REN` / `DEL`）。
  ボタンと右クリックのメニュー。終わったら一覧を読み直し、できなかった操作はまとめて知らせる。
  多数の削除は `RemoteDeleteQueue` で 8 件ずつ順に送る（`RemoteProtocol` の待ち行列をあふれさせない）。
  削除などの操作中は、移動・更新・パス入力を受け付けない
- **リモート内のコピー**（2026-11-14。sgnano の `COPY`、2026-10-24）: 右クリックの「コピー（リモート内）」でファイルを 1 つ選び、
  移動先のフォルダで「貼り付け」。元は FAT・フロッピー・exFAT・NTFS・データ CD、先は FAT・フロッピー・exFAT・NTFS。
  同じ名前は置き換える。**同じ exFAT の中の 4 MB 級のコピーは、本体が `read error` で失敗することがある**（積み残し #80。1.5 MB 級は成功）
- **結合と分割**（2026-11-14。sgnano の `JOIN` / `SPLIT`、2026-10-24）: 右クリックの「結合 (.001)…」で `name.001` から
  `.002`、`.003` … をつなぐ。「分割 (4 MB ずつ)…」で `name.001 …` を作る。どちらも同じフォルダの中で、同名は置き換える
- **S: の検査と修復**（2026-11-14。sgnano の `SYSFSCK`、2026-11-12）: S: を開いているときの右クリックから「S: を検査…」
  「S: を修復…」。ファイル・フォルダの数、直した大きさ・空きにした鎖・交差・壊れた鎖・FAT2 の数を出す
- `DRIVES` の追加の欄（USB の台番号 `msc`・区画の先頭 `plba`、2026-11-08）も読む。古い本体の 5 欄の形でも動く

## ビルド
```
cmake -B build -G Ninja && ninja -C build
```

## 試し（2026-11-14）
- `ctest --test-dir build --output-on-failure`: 実機なしの試しを CTest から実行する
- `build/sgnano_remote_selftest`: 実機なし。`RemoteProtocol` に答えを流し込み、DEL / REN / MKDIR の送る行（REN はタブ区切り）、OK / ERR の受け取り、
  断る形（空・制御文字・待ち行列がいっぱい）、待っていない OK を受け流すこと、並べた要求を 1 つずつ送ることを確かめる。
  2026-11-14 に足した `COPY`・`JOIN`・`SPLIT`・`SYSFSCK` の送る行と戻り、`DRV` の `msc` / `plba`、319 / 320 バイトの境目も確かめる。
  `RemoteDeleteQueue` の 8 件ずつの送信、34 件以上の完走、途中失敗、タイムアウト、同期的な返事も確かめる。
  S: の見分け（`RemoteFs::isSystemDrivePath`）と、システムのフォルダの守り（`RemoteFs::isProtectedSystemPath`）、`DRIVES` の S: の受け取りも確かめる
- `build/sgnano_remote_zmodem_selftest`: 実機なし。内蔵 ZMODEM の送る側と受け取る側を socketpair で向かい合わせ、
  複数ファイル・300 KB・空のファイル・逃がす文字（ZDLE / XON / XOFF / CR / 0x7F / 0xFF）の中身と名前・数を確かめる
- `build/sgnano_remote_uploadsplit_selftest`: 実機なし。分割の大きさの選び方（64 MB 以下は 1 MB、64 MB 超は 2 MB。`SGNANO_ZM_PART` があればその値）、
  4 MB を越えるファイルを 2 MB ずつに分けること、名前（.001 を含めて 131 バイト）、64 個の上限、`SGNANO_ZM_PART` の読み取りを確かめる
- `build/sgnano_remote_selftest --device /dev/ttyACM0`: 実機あり。`DRIVES` の種類で **HDD の exFAT（3）と NTFS（4）のうち、準備できた区画だけ**を選び（ドライブ文字はつなぐ順で変わる。無い種類は飛ばす）、
  作る・一覧で確かめる・空白を含む名前に変える・中身のあるフォルダの削除を断られる・消す・残っていないことを通す。作ったもの（`!qt_selftest`）は消してから終わる。
  S:（内蔵フラッシュ）があること、そのルートの 3 つのシステムのフォルダがシステムの属性で並ぶこと、
  `SYSFSCK`（検査だけ）が答えることも見る（消す・名前を変える試しはしない）
- `build/sgnano_remote_selftest --device-many /dev/ttyACM0`: 実機あり。同じく準備できた exFAT / NTFS に 34 個のフォルダを作り、`RemoteDeleteQueue` で 8 件ずつ消して、
  全部残らず消えたことを確かめる。作ったもの（`!qt_selftest_many`）は消してから終わる
- `build/sgnano_remote_selftest --device-large /dev/ttyACM0`: 実機あり。exFAT / NTFS のフォルダへ **4 MB + 123 バイト**のファイルをアップロードし、
  PC で 1 MB ずつに分ける → ZMODEM → 実機の `JOIN` → 分割ファイルの `DEL` まで通ることと、同じファイルをダウンロードして中身が一致することを確かめる。
  そのあと COPY（リモート内）と、実機の `SPLIT` / `JOIN` も見る。作ったもの（`!qt_selftest_large`）は消してから終わる。時間がかかる（4 MB 超を 2 回転送する）。
  JOIN の間、実機の「split / join: … KB」の表示が 65536 KB（64 MB）で止まる（積み残し #79。転送も 64 MB までで止まるという報告あり）。
  64 MB を越えるファイルは 2 MB ずつになる。2 MB で `no memory` になるときは `SGNANO_ZM_PART=1048576` で 1 MB ずつにできるが、`JOIN` は 64 個までなので 64 MB まで
- 実機の確認（2026-11-14、B: exFAT。この実機には NTFS 区画なし）: `--device`・`--device-large`・`--device-many` が成功。
  4 MB + 123 バイトの 1 MB 分割 → ZMODEM → JOIN → ダウンロード、64 KB の COPY / SPLIT / JOIN、34 フォルダの一括削除を確認

## 実機で残っていること（2026-11-14）
- **64 MB 超（できれば 500 MB）の単一ファイル**: 積み残し #79 の対応後。現状は ZMODEM の 1 ファイル 4 MB と JOIN の 64 個上限で不可
- **NTFS 区画での `--device` / `--device-large`**: この実機には NTFS 区画が無いため未実施
- **UI からの COPY / SPLIT / JOIN / SYSFSCK の手操作**: 遠隔コマンドとしては `--device` / `--device-large` で確認済み。画面からの操作は未確認
- **同一 exFAT 内の 4 MB 級 COPY**: 本体の read error（積み残し #80）。アプリ側では回避できない
