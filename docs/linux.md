# Linux

## ビルドとインストール

Debian/Ubuntu の場合:

```bash
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev libpcsclite-dev pcscd
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
sudo cmake --install build
sudo udevadm control --reload-rules
```

CMake の代わりに `make && make check && sudo make install` でも同じものが入ります
（[`../README.md`](../README.md) の「ビルド」）。

インストールされるもの:

| ファイル | 場所 |
|---|---|
| `mb8cfw-rec`、`mb8cfw-ctl` | `/usr/local/bin/` |
| udev ルール `60-mb8cfw.rules` | `/usr/local/lib/udev/rules.d/`（ディストリビューションによっては `/etc/udev/rules.d/` にコピーが必要） |
| IFD ハンドラ `ifd-mb8cfw.bundle` | pcsc-lite の `usbdropdir`（Debian なら `/usr/lib/pcsc/drivers/`） |

udev ルールで、ログイン中のユーザーと `video` グループが機器を開けるようになります。
録画サーバを別ユーザーで動かす場合は、そのユーザーを `video` グループに追加してください。

## CFW のロード

機器は挿すたびに純正ローダの状態で起動します。`mb8cfw-rec` に `--firmware` を渡しておけば、
CFW がまだ動いていないときは録画の前にロードします。手動でやる場合:

```bash
mb8cfw-ctl install frc1-m2tv.frc --dev m2tv
mb8cfw-ctl probe --dev m2tv
```

`probe` は読み出しだけで、hook と IDENTIFY の結果（ビルド ID、機種）を表示します。
`install` は純正の hook の 8 バイトを `frc1-m2tv.frc.hook` に保存するので、
コンテナと同じディレクトリに書き込み権限が必要です。

同じビルドの CFW が動いていれば、`install` も `mb8cfw-rec --firmware` も何も書きません。
別のプロセスが録画中かロード中のときは、最大 30 秒待ち、それでも空かなければ何も書かずにエラーで終わります。
録画中の機器に別のビルドを入れ直したいときは、先に録画を止めてください。

## 録画

引数は recpt1 と同じで、`<チャンネル> <秒数> <出力先>` の順です。秒数と出力先には `-` を指定できます。

```bash
mb8cfw-rec --dev m2tv --firmware frc1-m2tv.frc 27 10 out.ts
```

```bash
mb8cfw-rec --dev m2tv 27 - - | ffplay -
```

- 秒数が `-` のときは、SIGINT / SIGTERM を受けるか、パイプの読み手が閉じるまで録画を続けます。どの場合も `STOP` を送ってから終了します
- チャンネルが `-` のときは選局しません（今のチャンネルのまま流します）
- 地上波は 2 機種とも UHF 13〜50 ch（700 MHz 未満）です。CATV には対応していません
- PIX-DT295 は衛星も受信できます。チャンネルは `BS1`〜`BS23`（奇数）と `CS2`〜`CS24`（偶数）。
  BS は `/TS1` のように TS の番号（0〜7、既定は 0）か、`/0x4011` のように TSID を後ろに付けます。
  LNB への給電はしません
- CFW のコンテナは環境変数 `MB8CFW_FIRMWARE` でも指定できます
- 進捗や結果は標準エラーに出力するので、TS には混ざりません

### Mirakurun

`tuners.yml` の例です（Mirakurun での動作は未確認）。

```yaml
- name: GV-M2TV
  types: [GR]
  command: mb8cfw-rec --dev m2tv --quiet <channel> - -
  decoder: ...
```

Mirakurun のサービスに環境変数 `MB8CFW_FIRMWARE` を渡しておいてください。
2 台目は `--index 1` で指定します。

## 選局の確認

PIX-DT295:

```bash
mb8cfw-ctl tune 27 --dev dt295
mb8cfw-ctl scan --dev dt295           # 51 ch 以上は "not on this model"
mb8cfw-rec --dev dt295 27 60 out.ts
mb8cfw-ctl tune BS1/TS1 --dev dt295   # ロックした TSID も表示する
mb8cfw-ctl scan --sat --dev dt295     # BS と CS の全トランスポンダー
mb8cfw-rec --dev dt295 BS15 60 out.ts
```

GV-M2TV:

```bash
mb8cfw-ctl tune 27 --dev m2tv
mb8cfw-ctl scan --dev m2tv            # 51 ch 以上は "not on this model"
mb8cfw-ctl signal --dev m2tv
```

表示される C/N は復調 IC の生の値です（地上・衛星とも、大きいほど悪い）。
dB への換算式はまだありません。

## カードリーダー（PC/SC）

インストールすると、IFD ハンドラが pcsc-lite のドライバディレクトリに入ります。
pcscd は機器が挿さると、バンドルの `Info.plist` にある USB ID を見てこのハンドラを割り当てます。

```bash
sudo systemctl restart pcscd
pcsc_scan
```

`GV-M2TV CFW card slot` が表示され、カードありになって ATR が出れば成功です。
CFW が動いていない間、ハンドラは「カードなし」と応答し、hook を確認し直します（GV-M2TV は 3 秒ごと、カードを抜ける PIX-DT295 は 500 ms ごと）。

| `Info.plist` のキー | 既定値 | 意味 |
|---|---|---|
| `CFWAllowAPDU` | `false` | APDU を通すかどうか。`false` の間は電源、ATR、S(IFS) のみ |
| `CFWCardRemovable` | 機種による（DT295 のみ `true`） | カードを検出した後も 500 ms ごとに在否を確認するか |

APDU を通すには、インストールしたバンドルの `Contents/Info.plist` を書き換えて pcscd を再起動します。

```bash
sudo sed -i 's|<key>CFWAllowAPDU</key>\s*<false/>|<key>CFWAllowAPDU</key><true/>|' /usr/lib/pcsc/drivers/ifd-mb8cfw.bundle/Contents/Info.plist
```

キーと値が別の行にある場合、この `sed` では置き換わらないので手で直してください。
カードは私的利用の範囲で使ってください。

ログは syslog に `ifd-mb8cfw` の名前で出ます（`journalctl -t ifd-mb8cfw`）。

録画とカードは同時に使える作りです。録画は郵便受け 0、カードは郵便受け 1 を使い、
カード側は EP0 しか使わないのでインターフェースを占有しません。
同時に使えることは CFW のサンプルで確認しています。このドライバでの確認はまだです（[`../TODO.md`](../TODO.md)）。

## 確認済みの環境

Debian bookworm のコンテナ（aarch64）で、次を確認しました。

- CMake と `make` のどちらでも警告なしでビルドでき、テストがすべて通る
- `pcscd -f -d` がバンドルを読み込み（`Loading IFD Handler 3.0`）、関数をすべて解決する
  （コンテナには USB 機器が無いので、その先は未確認）

実機をつないだ Linux ではまだ動かしていません（[`../TODO.md`](../TODO.md)）。
