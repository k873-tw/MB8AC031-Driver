# Windows

Windows では BonDriver（`BonDriver_MB8CFW.dll`）と、コマンドラインツール `mb8cfw-ctl.exe`・`mb8cfw-rec.exe` が使えます。

## WinUSB の割り当て

純正ドライバの代わりに WinUSB を割り当て、libusb から WinUSB 経由で通信します。
割り当てると純正のソフトは使えなくなります。戻すときはデバイスマネージャーでドライバを元に戻してください。

1. [Zadig](https://zadig.akeo.ie/) を起動し、Options → List All Devices にチェックを入れる
2. 機器（GV-M2TV なら `04BB 053A`、PIX-DT295 なら `06B8 103B`）を選び、`WinUSB` をインストールする

## ビルド

どの方法でも、先に [libusb のリリース](https://github.com/libusb/libusb/releases)から
`libusb-1.0.xx.7z` を取ってきて展開しておきます。

### Visual Studio のソリューション

`libusb-1.0.xx.7z` をリポジトリ直下の `third_party/libusb/` に展開し、
`MB8CFW.sln` を開いてビルドします。別の場所に展開した場合は、環境変数 `LIBUSB_ROOT` を設定するか、
msbuild に `/p:LIBUSB_ROOT=...` を渡してください。

```bat
msbuild MB8CFW.sln /p:Configuration=Release /p:Platform=x64
```

出力は `build-msvc\x64\Release\` の `BonDriver_MB8CFW.dll`、`mb8cfw-ctl.exe`、`mb8cfw-rec.exe` です。

libusb のライブラリは `LIBUSB_ROOT\VS2022\MS64\static\libusb-1.0.lib`（Win32 なら `MS32`）を使います。
VS2019 用のものを使う場合は `/p:LIBUSB_VS=VS2019` を付けます。
設定は [`../msvc/mb8cfw.props`](../msvc/mb8cfw.props) にまとめてあります。

### CMake（Visual Studio）

開発者コマンドプロンプトで:

```bat
cmake -B build -A x64 -DLIBUSB_ROOT=C:\path\to\libusb-1.0.xx
cmake --build build --config Release
```

### MinGW（Linux、macOS からのクロスビルド）

CMake の場合:

```bash
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DLIBUSB_ROOT=/path/to/libusb-1.0.xx -DMB8_BUILD_TESTS=OFF
cmake --build build-win
```

make の場合:

```bash
make CROSS=x86_64-w64-mingw32- LIBUSB_ROOT=/path/to/libusb-1.0.xx
```

どちらも `LIBUSB_ROOT` の下の `include/libusb-1.0/libusb.h`（または `include/libusb.h`）と
`MinGW64/static/libusb-1.0.a` を使います。libgcc と libstdc++ は静的リンクするので、
DLL も exe も、依存するのは `KERNEL32.dll` と `msvcrt.dll` だけです。
Visual Studio と CMake + MSVC でも C ランタイムを静的リンク（`/MT`）するので、VC++ 再頒布可能パッケージは要りません。

### ビット数

BonDriver は、読み込むアプリと同じビット数でビルドしてください（exe はどちらでもかまいません）。
32 bit のアプリ（古い TVTest など）なら、Visual Studio では `Win32`、CMake では `-A Win32` を指定します。

## コマンドラインツール

使い方は Linux 版と同じです（[`linux.md`](linux.md) の「CFW のロード」「録画」「選局の確認」）。
コマンドプロンプトや PowerShell から実行します。

```bat
mb8cfw-ctl.exe list
mb8cfw-ctl.exe install frc1-m2tv.frc --dev m2tv
mb8cfw-ctl.exe probe --dev m2tv
mb8cfw-ctl.exe scan --dev m2tv
mb8cfw-rec.exe --dev m2tv --firmware frc1-m2tv.frc 27 30 out.ts
```

- `mb8cfw-rec.exe` の秒数を `-` にしたときは、Ctrl+C か Ctrl+Break で止めます。コンソールウィンドウを閉じた場合も、`STOP` を送ってから終了します
- 出力先を `-` にすると標準出力にバイナリのまま書き出すので、`mb8cfw-rec.exe 27 - - | ffplay -` のようにパイプでつなげます
- CFW のコンテナは環境変数 `MB8CFW_FIRMWARE` でも指定できます（`set MB8CFW_FIRMWARE=C:\tv\frc1-m2tv.frc`）
- `mb8cfw-ctl.exe install` は純正の hook を `<コンテナ>.hook` に保存するので、コンテナを書き込みできるフォルダに置いてください

WinUSB は 1 つのデバイスを 1 プロセスからしか開けません。そのため、TVTest などで BonDriver が機器を開いている間は、
ツールからは開けないはずです（未確認）。ツールを使うときはアプリ側でチューナーを閉じてください。
Linux と違い、録画中にカードを別プロセスから使うこともできません。

## BonDriver の配置

アプリの BonDriver フォルダに、次のファイルを置きます。

| ファイル | 説明 |
|---|---|
| `BonDriver_MB8CFW.dll` | ビルドしたもの |
| `BonDriver_MB8CFW.ini` | [`../src/windows/bondriver/BonDriver_MB8CFW.ini`](../src/windows/bondriver/BonDriver_MB8CFW.ini) をコピーして編集する |
| `frc1-m2tv.frc` など | MB8AC031-CFW でビルドしたコンテナ |

ini ファイルは DLL と同じ名前のものが読まれます。DLL を `BonDriver_MB8CFW_2.dll` という名前で
コピーすると `BonDriver_MB8CFW_2.ini` を読むので、2 台目はそちらで `Index=1` にします。

| キー | 既定値 | 説明 |
|---|---|---|
| `Device` | 最初に見つかった機器 | `m2tv`、`dt295`、または `vvvv:pppp` |
| `Index` | `0` | 同じ機種の何台目を使うか |
| `Firmware` | なし | CFW のコンテナ。CFW が動いていなければ、開くときにロードする |
| `LockWaitMs` | `2000` | 選局後にロックを待つ時間 |
| `BufferMiB` | `32` | ホスト側の TS のリングバッファの大きさ |
| `Name` | `MB8AC031-CFW` | アプリに表示されるチューナー名 |

相対パスは DLL の場所を基準にします。

## チャンネル

GV-M2TV:

| 空間 | チャンネル |
|---|---|
| 0 `UHF` | 13ch〜50ch（700 MHz 未満。CATV は無し） |

IBonDriver（v1）の `SetChannel(BYTE)` では、13〜50 を UHF のチャンネルとして受け付けます。

PIX-DT295 は空間が多く、開いた後に一覧が切り替わります。

| 空間 | チャンネル |
|---|---|
| 0 `UHF` | 13ch〜50ch（700 MHz 未満。CATV は無し） |
| 1 `BS` | BS01/TS0〜BS23/TS3。1 つのトランスポンダーに 4 つ（TS の番号）。実際に載っていない番号はロックしない |
| 2 `CS110` | ND02〜ND24 |

LNB への給電はしません。

`GetSignalLevel` は、ロックしていれば `1.0`、していなければ `0.0` を返します。
C/N を dB に換算する式がまだ無いためです。

## ログ

`OutputDebugString` に `BonDriver_MB8CFW:` を頭に付けて出力します。
[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) で見られます。

## 実装上の注意

`GetTsStream` は 2 つあります。BonDriver の vtable の並びは MSVC の規則で決まっていて、
MSVC は同名の仮想関数を宣言と逆の順に並べます。GCC と Clang は宣言順に並べるので、
[`IBonDriver.h`](../src/windows/bondriver/IBonDriver.h) では MinGW 向けに宣言の順番を入れ替えています。
MinGW でビルドした DLL を逆アセンブルして、`GetTsStream(BYTE **)` が 7 番目（`+0x30`）、
`GetTsStream(BYTE *)` が 8 番目（`+0x38`）、`GetCurChannel` が 17 番目（`+0x80`）になっていることを確認しました（2026-11-14）。

DLL がエクスポートするのは `CreateBonDriver` だけです。

## 確認済みの範囲

- MinGW（Debian bookworm の `mingw-w64`）で、CMake と `make` のどちらでも DLL と exe が警告なしでビルドできる
- vtable の並びは上のとおり
- ただし libusb は中身の無いスタブでリンクを確かめただけです。本物の libusb と Windows の実機では、DLL も exe もまだ動かしていません
- Visual Studio のソリューションは、まだ Visual Studio で開いてビルドしていません（[`../TODO.md`](../TODO.md)）
