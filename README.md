# MB8AC031-Driver

MB8AC031-CFW を載せたテレビチューナーを PC から使うためのドライバです。
ライセンスは [MIT](LICENSE)。
BonDriver のインターフェース（[`IBonDriver.h`](src/windows/bondriver/IBonDriver.h) の `IBonDriver`・`IBonDriver2`）は、
TVTest などが呼び出す ABI に合わせて宣言を書き直したもので、これも MIT です。

中身は次のとおりです。

- `libmb8cfw`（[`src/lib/`](src/lib/)）: 本体のライブラリ。機器を開いて CFW をロードし、
  `FRQ1` の枠で選局・TS の受信・カードとのやりとりをする。C11 と libusb-1.0 で書いている
- コマンドラインツール（Linux、macOS、Windows 共通）: 録画コマンド `mb8cfw-rec`（recpt1 と同じ引数）と、
  CFW のロード・選局・信号の確認をする `mb8cfw-ctl`
- Linux 向け: PC/SC の IFD ハンドラ（カードリーダー）、udev ルール
- Windows 向け: `BonDriver_MB8CFW.dll`（TVTest・EDCB など）

機器側のファームウェアは [MB8AC031-CFW](https://github.com/soralis0912/MB8AC031-CFW) です。
このドライバが話すのは、その CFW が受け付ける電文（`FRQ1` の枠、TS の出し方、
カードの枠）だけで、純正のファームウェアやドライバとは通信しません。

## 対応機種と現状

| 機種 | USB ID | 系統 | TS | 選局 | カード |
|---|---|---|---|---|---|
| GV-M2TV | `04bb:053a` | dione | 実機で確認 | 地上 UHF 13〜50 ch | 未確認 |
| PIX-DT295 | `06b8:103b` | selene | 実機で確認 | 地上 UHF 13〜50 ch、BS、CS110 | NG（`CARD_ON` が `E_IO`） |

2026-11-14 に、2 機種とも macOS 上でこのドライバから選局して TS を録画できました。
カードは、CFW のサンプル（CFW の `sample/`）で 2 機種とも実機が通った手順をそのまま使っていますが、
このドライバからは、GV-M2TV は未確認、PIX-DT295 は `CARD_ON` が `E_IO` になって通っていません。詳しくは [`TODO.md`](TODO.md) の 0 節。

700 MHz 以上と CATV には対応していません（`MB8_E_UNSUPP` を返します）。
選局の手順はライブラリに組み込んであるので、別途テーブルファイルを用意する必要はありません。
各レジスタの書き込みは、実機で 1 つずつ効果を確かめたものです
（[`docs/tuning-m2tv.md`](docs/tuning-m2tv.md)、[`docs/tuning-dt295.md`](docs/tuning-dt295.md)）。
PIX-DT295 は LNB への給電をしません。衛星は、別途給電したアンテナで確認しました。

ほかの機種は、CFW で実機の動作が確認できてから追加します。

## ビルド

ビルド方法は 3 通りあります。どれでも同じものができます。

| 方法 | 向いている環境 | できるもの |
|---|---|---|
| CMake | Linux、macOS、Windows（VS・MinGW） | すべて |
| `make`（[`Makefile`](Makefile)） | Linux、macOS、MinGW でのクロスビルド | すべて |
| Visual Studio（[`MB8CFW.sln`](MB8CFW.sln)） | Windows | ライブラリ、`mb8cfw-ctl.exe`、`mb8cfw-rec.exe`、BonDriver |

### 必要なもの

- C11 のコンパイラ（GCC、Clang、Visual Studio 2022）
- libusb-1.0
  - Debian/Ubuntu: `sudo apt install libusb-1.0-0-dev`
  - macOS: `brew install libusb`
  - Windows: [libusb のリリース](https://github.com/libusb/libusb/releases)の
    `libusb-1.0.xx.7z` を展開しておく
- IFD ハンドラを作るなら pcsc-lite の開発用ヘッダ（`libpcsclite-dev`）。
  見つからなければ IFD ハンドラは作らずに進みます
- CMake を使うなら 3.16 以上、`make` を使うなら GNU make と pkg-config

### CMake

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

`ctest` は機器をつながなくても実行できます。

オプション:

| オプション | 既定値 | 内容 |
|---|---|---|
| `MB8_BUILD_TOOLS` | `ON` | `mb8cfw-rec`、`mb8cfw-ctl`（Windows では `.exe`） |
| `MB8_BUILD_IFD` | `ON` | IFD ハンドラ（Windows では作らない） |
| `MB8_BUILD_BONDRIVER` | Windows なら `ON` | BonDriver |
| `MB8_BUILD_TESTS` | `ON` | テスト |
| `LIBUSB_ROOT` | なし | Windows で libusb を展開した場所 |

### make

```bash
make
make check
sudo make install
```

成果物は `build-make/` に出ます。

| 変数 | 既定値 | 内容 |
|---|---|---|
| `PREFIX` | `/usr/local` | インストール先 |
| `DESTDIR` | なし | パッケージを作るときの一時ルート |
| `BUILD` | `build-make` | 出力先 |
| `CROSS` | なし | クロスコンパイラの接頭辞（例: `x86_64-w64-mingw32-`） |
| `LIBUSB_ROOT` | なし | `CROSS` を使うときの libusb の場所 |

pcsc-lite が pkg-config で見つかったときだけ IFD ハンドラ（`build-make/ifd-mb8cfw.bundle`）を作ります。
`make install` は pcsc-lite の `usbdropdir` にバンドルを置きます。

Windows 向けに Linux や macOS からクロスビルドする場合:

```bash
make CROSS=x86_64-w64-mingw32- LIBUSB_ROOT=/path/to/libusb-1.0.xx
```

`build-make/` に `mb8cfw-ctl.exe`、`mb8cfw-rec.exe`、`BonDriver_MB8CFW.dll`、`BonDriver_MB8CFW.ini` ができます。
libusb は `$LIBUSB_ROOT/MinGW64/static/libusb-1.0.a` を静的リンクします。
BonDriver だけでよければ `make bondriver CROSS=...` です。

### Visual Studio

1. libusb の `libusb-1.0.xx.7z` を `third_party/libusb/` に展開する
   （`third_party/libusb/include/` と `third_party/libusb/VS2022/` ができる状態）。
   別の場所に置いた場合は、環境変数 `LIBUSB_ROOT` にそのパスを入れておく
2. `MB8CFW.sln` を開き、構成（Release）とプラットフォーム（x64 か Win32）を選んでビルドする

コマンドラインなら、開発者コマンドプロンプトで:

```bat
msbuild MB8CFW.sln /p:Configuration=Release /p:Platform=x64 /p:LIBUSB_ROOT=C:\libusb-1.0.xx
```

成果物（`mb8cfw-ctl.exe`、`mb8cfw-rec.exe`、`BonDriver_MB8CFW.dll`）は `build-msvc\x64\Release\` に出ます。
`BonDriver_MB8CFW.ini` も同じ場所にコピーされます。
BonDriver は使うアプリと同じビット数で作ってください（32 bit の TVTest なら `Win32`）。

ソリューションに入っているのは `mb8cfw`（静的ライブラリ）、`mb8cfw-ctl`、`mb8cfw-rec`、`BonDriver_MB8CFW` の 4 つです。
テストは入っていないので、Windows でテストを回すなら CMake を使ってください。

インストールと使い方は、Linux が [`docs/linux.md`](docs/linux.md)、
Windows が [`docs/windows.md`](docs/windows.md) にあります。

## 使う前に用意するもの

CFW のコンテナ `frc1-<機種>.frc` は、このリポジトリには入っていません。
MB8AC031-CFW で `python3 host/build.py frc1` を実行して作ります。
機器は挿すたびに純正ローダの状態で起動するので、ドライバが開くたびに CFW を RAM へロードします。

## ドキュメント

| ファイル | 内容 |
|---|---|
| [`docs/README.md`](docs/README.md) | 内部の構成（スレッド、郵便受け、枠の流れ） |
| [`docs/linux.md`](docs/linux.md) | Linux でのインストール、録画、カードリーダー |
| [`docs/windows.md`](docs/windows.md) | WinUSB の割り当て、コマンドラインツール、BonDriver の設定 |
| [`docs/tuning-m2tv.md`](docs/tuning-m2tv.md)、[`docs/tuning-dt295.md`](docs/tuning-dt295.md) | 選局で書くレジスタと、それぞれの役割 |
| [`TODO.md`](TODO.md) | 実機での確認状況と残り |

## B-CAS カードについて

カードスロットに挿すのは B-CAS カードです。私的利用の範囲で扱ってください。
IFD ハンドラは、既定では APDU を通しません（電源、ATR、S(IFS) のみ）。
通す場合は、インストールしたバンドルの `Info.plist` で `CFWAllowAPDU` を `true` にします
（[`docs/linux.md`](docs/linux.md)）。
