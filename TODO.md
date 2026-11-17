# TODO

## 0. 確認状況

PIX-DT295 と GV-M2TV の実機で動作しました（2026-11-14、macOS。CFW のビルド ID は DT295 が
`0x83fa2176`、GV-M2TV が `0x7162c726`）。

| 項目 | 結果 |
|---|---|
| macOS（clang）でビルド、テスト 7 本 | OK（CMake、`make` とも） |
| Linux（Debian bookworm のコンテナ、aarch64）でビルド、テスト 7 本 | OK。警告なし（CMake、`make` とも） |
| IFD ハンドラを `pcscd` が読み込む | OK（コンテナ内、実機なし） |
| BonDriver を MinGW でビルド | OK（CMake、`make` とも）。libusb はスタブで、リンクの確認のみ。BS・CS110 の空間を追加した版もコンパイルは通る |
| `mb8cfw-ctl.exe`、`mb8cfw-rec.exe` を MinGW でビルド | OK（CMake、`make` とも）。libusb はスタブ。Windows では未実行 |
| BonDriver、`mb8cfw-ctl.exe`、`mb8cfw-rec.exe` を Visual Studio（`MB8CFW.sln`）でビルド | 未確認 |
| PIX-DT295: `mb8cfw-ctl tune`（強いチャンネルと弱いチャンネル）、`signal` | OK。別の設定が残った状態からでもロックする。51 ch 以上は受け付けない |
| PIX-DT295: `mb8cfw-rec` 強いチャンネル 10 秒、弱いチャンネル 60 秒 | OK。同期の取り直し・飛び・TEI が 0、PAT あり |
| PIX-DT295: `mb8cfw-rec` を SIGTERM で止めてから開き直す | OK |
| PIX-DT295: `mb8cfw-ctl scan --sat` | OK。受信できる BS・CS110 のトランスポンダーがすべてロック。衛星側のレジスタが別のソフトで変えられたままの状態からでもロックする |
| PIX-DT295: `mb8cfw-ctl tune BS1/TS1`、`BS1/TS2`、`BS15/0x40F2` | OK。指定した TSID にロックする。載っていない `BS1/TS5` はロックしない |
| PIX-DT295: `mb8cfw-rec` で BS1/TS1・CS2 を 10 秒、続けて地上 10 秒、BS15 を 60 秒 | OK。崩れ 0。衛星の PAT の TSID が指定どおり（`0x4011`、`0x6020`、`0x40F1`）、地上も PAT あり |
| PIX-DT295: `mb8cfw-ctl card` | NG。`CARD_ON` が `E_IO` になり、在否のビットも 0。カードが挿さっていたかは未確認 |
| GV-M2TV: `mb8cfw-ctl install`、`probe` | OK |
| GV-M2TV: `mb8cfw-ctl scan`（組み込みの手順、テーブルファイルなし） | OK。受信できるチャンネルがすべてロック。51 ch 以上は受け付けない |
| GV-M2TV: 電源を入れ直した直後から `mb8cfw-rec` で 2 チャンネル | OK。同期の取り直し・飛び・TEI が 0、PAT あり |
| GV-M2TV: `mb8cfw-rec` 弱いチャンネル 60 秒 | 4 回中 3 回は崩れ 0。1 回だけ TEI 12、飛び 3（最も弱いチャンネル） |
| GV-M2TV: `mb8cfw-rec` を SIGTERM で止める、読み手がパイプを閉じる | OK（どちらも終了コード 0） |
| GV-M2TV: カード | 未確認 |

## 1. 実機で確認すること

テスト完了の条件（CFW を載せた実機から生の TS が取れること、PC からカードリーダーとして認識されて ATR が取れること）に照らして確認する。

- [ ] 2026-11-17 の CFW（未使用のリセットの削除など。ホストとの電文は変わらない）で、2 機種ともドライバから選局・録画をやり直し、上の 0 節のビルド ID を更新する
- [x] GV-M2TV: `mb8cfw-ctl install` → `probe`
- [x] GV-M2TV: `mb8cfw-ctl tune`、`scan`（組み込みの手順）
- [x] GV-M2TV: `mb8cfw-rec <ch> 60 out.ts`。同期の取り直し 0、連続性カウンタの飛び 0、PAT が読める
- [x] GV-M2TV: `mb8cfw-rec … - - | head -c …` でパイプが閉じられたときと `kill -TERM` のときに、STOP が送られる
- [ ] GV-M2TV: 弱いチャンネルを長時間録画して、TEI がどれくらい出るか確認する（60 秒で 12 が 1 回あった）
- [ ] 録画しながら `pcsc_scan` で ATR が取れる（郵便受けを分けてあるので動くはず）
- [ ] IFD ハンドラを入れた `pcscd` で、カードありになって ATR が出る
- [ ] Windows: WinUSB を割り当て、TVTest で BonDriver を開いて選局し、映像が出る
- [ ] Windows: 本物の libusb でビルドする（Visual Studio の `MB8CFW.sln`、CMake + MSVC、MinGW）
- [ ] Windows: `mb8cfw-ctl.exe install`・`probe`・`tune`、`mb8cfw-rec.exe` で録画し、Ctrl+C で `STOP` が送られること
- [ ] 2 つのプロセスから同時にロードする（CFW が無い状態で `mb8cfw-rec --firmware` を 2 つ同時に起動）。後のほうが待ってから「this build went in meanwhile」で何も書かず、先のほうの録画が崩れないこと。macOS と Linux の両方で
- [ ] 録画中に別のプロセスから `mb8cfw-ctl install --force` を打ち、30 秒後に `MB8_E_BUSY` で何も書かずに終わること
- [ ] Windows: BonDriver を開いている間に `mb8cfw-ctl.exe` を使うとどうなるか（WinUSB は 1 プロセスしか開けないはず）
- [x] PIX-DT295: 地上波（UHF）の選局と録画（上の表）
- [x] PIX-DT295: 衛星（BS・CS110）の選局と録画（上の表）
- [ ] PIX-DT295: 衛星を Windows（BonDriver）で
- [ ] PIX-DT295: カード。`CARD_ON` が `E_IO` になった。CFW のサンプル（`ifd`）で通る手順との違いを調べる
- [ ] PIX-DT295: Linux でビルドし、IFD ハンドラと `mb8cfw-rec` を同時に使う

## 2. 未対応のもの

- 700 MHz 以上（PIX-DT295 は、バンド切り替えの書き込みを受信できるチャンネルで確認する必要がある）
- GV-M2TV の CATV（パススルー）。332 MHz 未満でのチューナー `0x80` の切り替えを、ケーブルで確認する
- C/N を dB に換算する式（CFW の `docs/demod.md` の「dB への換算」）
- Windows のカードリーダー（UMDF のスマートカードドライバか、仮想 CCID）
- 同じ機種を 2 台つないだとき、IFD ハンドラが pcscd の割り当て順に合わせて機器を開くこと
  （今は「同じ機種の n 番目のスロット = n 番目の機器」としている）
- PIX-DT295 の LNB 給電（どこで切り替えるのか分かっていない）
