# ビルドと一括テスト

リポジトリ直下で実行する。C++17対応のC++コンパイラ、GNU Make 3.81以降、Python 3.10以降を使用する。Python側は標準ライブラリのみ。GUI操作テストにはデスクトップとTk、macOSではTk 8.6以降が必要。

```sh
make                 # ./main をビルド
make -j3 test        # 通常の回帰テスト一式（実サーバー通信なし）
make test-sanitize   # ASan/UBSanによるC++テスト
```

## Windows

Windows 10/11ではCMake 3.20以降、Python 3.10以降と、次のいずれかのC++17環境を使用する。

- Visual Studio 2022 Build Toolsの「C++によるデスクトップ開発」
- MinGW-w64 GCC
- Windows向けClang

PowerShellから次を実行すると、`.build/windows/`へビルド構成を作り、リポジトリ
直下へ`main.exe`を出力する。

```powershell
./build-windows.ps1
./build-windows.ps1 -Test
./run-visualizer-windows.ps1
py -3 visualizer/app.py
```

Visual Studioを使う場合はDeveloper PowerShell、MinGW/Clangを使う場合は
コンパイラとCMakeへPATHが通ったPowerShellで実行する。ジェネレータを明示する
場合は、たとえば`./build-windows.ps1 -Generator "Visual Studio 17 2022"`とする。
スクリプトを使わない場合の同等コマンドは次のとおり。

```powershell
cmake -S . -B .build/windows
cmake --build .build/windows --config Release --target procon_solver
ctest --test-dir .build/windows -C Release --output-on-failure
```

Windowsでは実行ファイル名が`main.exe`になり、GUIと通信クライアントもこの名前を
既定で使用する。ASan/UBSan用の`make test-sanitize`はGCC/Clang系Unix環境向けで、
MSVCではCMakeの通常回帰テストを使用する。

Windowsの実行ファイルとテストには16MBのスタック予約を設定する。終了コード
`3221225725`（`0xC00000FD`）が出た場合は古い`main.exe`が残っているため、
`build-windows.ps1`またはGUIの「mainをビルド」で再ビルドする。
`build-windows.ps1 -Test`ではPython 3.10以降も必須とし、見つからない場合は
テストを黙って省略せずCMake設定を失敗させる。

既定ビルドはC++17、`-O2 -pthread -Wall -Wextra -Wpedantic -Werror`。ヘッダー依存関係を自動記録するので、ヘッダー変更も次のビルドへ反映される。テストは`-UNDEBUG`でassertを有効にする。ビルド成果物とPythonキャッシュはGit管理外の`.build/`へ置き、ソルバーだけはGUI・CLIの既定パス`./main`へ出力する。ビルド・テスト中に本番の自動実行を同時に動かさない。

CMake版も同じC++17を使用し、Windowsでは`Threads::Threads`とMSVCの
`/W4 /EHsc /utf-8`、GCC/Clangでは従来の警告エラー化オプションを選択する。MSVCの
警告体系はGCC/Clangと異なるため、移植時の追加警告をビルド失敗にはしない。

## ターゲット

| コマンド | 内容 |
|---|---|
| `make` / `make release` | ソルバーの最適化ビルド |
| `make test` | C++モデル・探索・引数・3サイズサンプル、Pythonバックエンド・復旧・起動環境。実ソルバーとの4日間連携も実行 |
| `make test-core` | サンプルJSONを必要としないC++・Pythonテスト |
| `make test-cpp` | C++11テスト（系列最大化・連鎖再計画・玉数ALNS・日跨ぎ候補・公式Q&A・プロパティ・サンプルを含む） |
| `make test-brands` | 系列最大化を独立した全列挙と照合し、ビームの取りこぼし補完を検証 |
| `make test-brand-repair` | 役割交換、補給、固定行動、巻き戻しの系列救済を検証 |
| `make test-udon-alns` | 系列マスクを固定したALNSの玉数改善と非退行を検証 |
| `make test-quantity-search` | 途中取得、時刻付き補給、日跨ぎ候補と中断時の元計画保持を検証 |
| `make test-python` | Python 55テスト。ソルバーを自動ビルドして連携テストのスキップを防止 |
| `make test-gui` | ウィンドウを表示するGUI操作テスト。通信はモック |
| `make test-server` | 同梱簡易サーバーをlocalhostで起動し、実ソルバーで探索・送信 |
| `make test-sanitize` | モデル・探索・引数・公式Q&A・プロパティテストをASan/UBSanでビルド・実行 |
| `make test-all` | 通常・GUI・簡易サーバー・サニタイザをすべて実行 |
| `make tools` | ベンチマークと重み調整ツールを`.build/tools/`へビルド（実行しない） |
| `make help` | コマンド一覧 |

通常テストはGUIを表示せず、ネットワーク接続も行わない。`make test`にはGit管理外の`sample_maps/sample_16.json`、`sample_24.json`、`sample_32.json`が必要。欠けている場合はエラーで終了し、黙ってスキップしない。サンプル未配置の環境では`make test-core`を使う。`make test-server`には同梱サーバーと設定JSON、localhostで待ち受けできる権限が必要。いずれかのテストが失敗するとmakeも非ゼロで終了する。

2026-10-04時点で`make test`、`make test-sanitize`、`make test-python`の成功を確認している。`make test-python`はPython 36件を実行する。`make test-gui`はデスクトップと対応Tkを持つ環境で別途実行し、同日時点で1件の成功を確認している。

2026-10-05の経路候補組合せ追加後には`make test-core`が成功した。追加した`spot_route_lns_test`の経路候補組合せテストはASan/UBSanでも通過した。この変更について`make test`・`make test-sanitize`全体やGUI実画面を再実行したという意味ではない。

2026-10-07の協調改善と公開得点追跡改善後に、`make test`（C++ 11テスト、公開3サイズ、Python 38件）、`make test-sanitize`（C++ 10テスト）、`git diff --check`が成功した。GUI実画面と簡易サーバーはこの変更後に再実行していない。

2026-10-08の5台時エージェント構成改善後に、`make test-core`（C++コアテストとPython 39件）、`make test-sanitize`（C++ 10テスト）、公開3サイズの`sample_maps_test`、`git diff --check`が成功した。GUI実画面、簡易サーバー、公式実サーバーへの回答送信はこの変更後に実行していない。

2026-10-09の3人運用更新後に、`make test-core`（C++コアテストとPython 53件）、`make test-gui`（実画面1件）、`git diff --check`が成功した。問題・状態の公開待ち、担当別プリセット、担当1の全自動、追走端末の手動送信、受理済みチェックポイントの共有と最大revision取込を含む。公式実サーバーへの回答送信を実行したという意味ではない。

同日の現行GUI更新後は、担当プリセットを15／35／45秒へ変更し、探索終了後の伝達3値表示と口頭3値の取込を追加した。`make -j3 test-core`ではPython 57件を含む回帰テスト、`make test-gui`では実画面1件が成功した。

2026-10-10の`windows`ブランチ更新では、Windows向けコマンド生成、`main.exe`既定パス、同梱Windows版簡易サーバー、CMake／PowerShellビルドを`main`の`5dcc8ab`まで統合した。接続テストの全日ダミー送信と、担当別の予行演習実探索・実送信もWindows GUIへ反映した。CMakeのPython回帰にも1969履歴テストを追加し、Mac上の回帰テストとWindows向けコマンド生成テストが成功した。このMacにはCMakeがないためCMake経路とWindows実機ビルドは未実施であり、本番前の物理3台リハーサルで別途確認する。

## 2026-10-10 Windowsへの第1試合・共同経路改善の反映

`main`の`1c81302`を取り込み、第1試合の担当1約5秒・担当2約35秒・担当3約45秒と、16×16最終日の交通・補給時刻を使う共同経路改善を反映した。`main.exe`、MSVCの`/O2 /DNDEBUG /utf-8`、MinGWの`-O3 -DNDEBUG`・静的C++ランタイム、16MBスタックは維持する。担当設定はビジュアライザで実行時に選ぶ。macOS限定の選択欄縮小はWindowsには適用しない。系列／系列累計／総獲得数はワークフロー見出しの右側に表示する。

新しい対比較ツールもWindows用にビルドできる。通常のビルドには含めず、必要時だけ次を実行する。

```powershell
./build-windows.ps1 -Benchmark
./.build/windows/tools/quantity_joint_benchmark.exe 9 8 0 mixed
```

CMakeを直接使う場合は`-DPROCON_BUILD_BENCHMARK=ON`で有効化する。実行時間は探索時間の比較枠であり、ビルド時間ではない。Windows実機・MSVC／MinGWでの実行確認は別途必要である。

## 2026-10-11 最新mainと準決勝条件の反映

`main`の`89edeb9`までを`windows`へ取り込み、系列完了ボーナス、7台時の系列重視エージェント選択、準決勝の確定条件、エージェント種別の初期・中間・最終3段階提出を反映した。種別探索上限は第1試合50秒、準決勝80秒、決勝110秒である。`std::clamp`と`std::from_chars`を使うヘッダーの直接依存も補い、CMakeとGUIのどちらもC++17でビルドする。Windowsでは引き続き`main.exe`を使用し、準決勝・決勝は1人・1端末、3人運用「通常」で実行する。準決勝の設定取得後は24×24、6日、日別ステップ`48,56,68,68,56,72`、5台、24スポット、18系列、最大在庫合計60、燃料80、混雑1、渋滞2を完全一致で検査する。

本番用ビルドと回帰テストはDeveloper PowerShellまたはMinGW／ClangのPATHを設定したPowerShellで次を実行する。

```powershell
./build-windows.ps1 -Configuration Release -Test
./run-visualizer-windows.ps1 --check-runtime
```

このブランチを更新したmacOS環境にはWindows用コンパイラ、CMake、PowerShellがないため、ネイティブ`main.exe`の生成とWindows GUI実画面確認はWindows端末で行う。
macOS上ではWindows向けビルドコマンド検査を含むPython回帰74件とC++コア回帰が成功している。

## コンパイラ等の指定

```sh
make -B CXX=clang++ PYTHON=python3
make -B CXXFLAGS='-O2 -Wall -Wextra -Wpedantic -Werror'
```

`CXX`、`PYTHON`、`CPPFLAGS`、`CXXFLAGS`、`LDFLAGS`、`LDLIBS`を指定できる。コンパイラやフラグの変更だけでは再ビルドを検出しないため、変更時は`-B`を付ける。サニタイザ用には通常バイナリと別の出力先・専用フラグを使用する。

本番前には通常・GUI・簡易サーバー・サニタイザを通し、使用したコミット、コンパイラ、フラグを記録して`main`を別途バックアップする。これらのテストは公式実サーバーでのリハーサルを代替しない。

WindowsのMinGWビルドでは、C++ランタイムを-static-libgcc -static-libstdc++で静的リンクする。PATH上のランタイムDLLによるスレッド終了時のヒープ破損（0xC0000374）を避けるため、CMakeとGUIのビルドで共通に指定する。設定変更後はmain.exeを再ビルドする。
