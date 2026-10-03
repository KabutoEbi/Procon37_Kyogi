# ビルドと一括テスト

リポジトリ直下で実行する。C++17対応のC++コンパイラ、GNU Make 3.81以降、Python 3.10以降を使用する。Python側は標準ライブラリのみ。GUI操作テストにはデスクトップとTk、macOSではTk 8.6以降が必要。

```sh
make                 # ./main をビルド
make -j3 test        # 通常の回帰テスト一式（実サーバー通信なし）
make test-sanitize   # ASan/UBSanによるC++テスト
```

既定ビルドはC++17、`-O2 -pthread -Wall -Wextra -Wpedantic -Werror`。ヘッダー依存関係を自動記録するので、ヘッダー変更も次のビルドへ反映される。テストは`-UNDEBUG`でassertを有効にする。ビルド成果物とPythonキャッシュはGit管理外の`.build/`へ置き、ソルバーだけはGUI・CLIの既定パス`./main`へ出力する。ビルド・テスト中に本番の自動実行を同時に動かさない。

## ターゲット

| コマンド | 内容 |
|---|---|
| `make` / `make release` | ソルバーの最適化ビルド |
| `make test` | C++モデル・探索・引数・3サイズサンプル、Pythonバックエンド・復旧・起動環境。実ソルバーとの4日間連携も実行 |
| `make test-core` | サンプルJSONを必要としないC++・Pythonテスト |
| `make test-cpp` | C++9テスト（系列最大化・連鎖再計画・玉数ALNS・公式Q&A・プロパティ・サンプルを含む） |
| `make test-brands` | 系列最大化を独立した全列挙と照合し、ビームの取りこぼし補完を検証 |
| `make test-brand-repair` | 役割交換、補給、固定行動、巻き戻しの系列救済を検証 |
| `make test-udon-alns` | 系列マスクを固定したALNSの玉数改善と非退行を検証 |
| `make test-python` | Python 36テスト。ソルバーを自動ビルドして連携テストのスキップを防止 |
| `make test-gui` | ウィンドウを表示するGUI操作テスト。通信はモック |
| `make test-server` | 同梱簡易サーバーをlocalhostで起動し、実ソルバーで探索・送信 |
| `make test-sanitize` | モデル・探索・引数・公式Q&A・プロパティテストをASan/UBSanでビルド・実行 |
| `make test-all` | 通常・GUI・簡易サーバー・サニタイザをすべて実行 |
| `make tools` | ベンチマークと重み調整ツールを`.build/tools/`へビルド（実行しない） |
| `make help` | コマンド一覧 |

通常テストはGUIを表示せず、ネットワーク接続も行わない。`make test`にはGit管理外の`sample_maps/sample_16.json`、`sample_24.json`、`sample_32.json`が必要。欠けている場合はエラーで終了し、黙ってスキップしない。サンプル未配置の環境では`make test-core`を使う。`make test-server`には同梱サーバーと設定JSON、localhostで待ち受けできる権限が必要。いずれかのテストが失敗するとmakeも非ゼロで終了する。

2026-10-04時点で`make test`、`make test-sanitize`、`make test-python`の成功を確認している。`make test-python`はPython 36件を実行する。`make test-gui`はデスクトップと対応Tkを持つ環境で別途実行し、同日時点で1件の成功を確認している。

## コンパイラ等の指定

```sh
make -B CXX=clang++ PYTHON=python3
make -B CXXFLAGS='-O2 -Wall -Wextra -Wpedantic -Werror'
```

`CXX`、`PYTHON`、`CPPFLAGS`、`CXXFLAGS`、`LDFLAGS`、`LDLIBS`を指定できる。コンパイラやフラグの変更だけでは再ビルドを検出しないため、変更時は`-B`を付ける。サニタイザ用には通常バイナリと別の出力先・専用フラグを使用する。

本番前には通常・GUI・簡易サーバー・サニタイザを通し、使用したコミット、コンパイラ、フラグを記録して`main`を別途バックアップする。これらのテストは公式実サーバーでのリハーサルを代替しない。
