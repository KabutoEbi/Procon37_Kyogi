# Procon37 solver manual

このディレクトリは、ソルバー本体、GUI、テスト、回答システム用クライアントの使い方を実装ファイル単位でまとめる。初めて使う場合は[ルートREADME](../README.md)から読む。

現行仕様の最終照合日は2026-10-04。系列救済型の連鎖再計画、系列固定の玉数ALNS、32×32向けの種別・経路選択、全盤面・全台数の最終日経路比較、補給車経路安定化、ビジュアライザのランダム生成既定値、日別探索時間、JSON表示を反映している。日付を含む`tests/*-results-YYYY-MM-DD.md`は、その日時点の条件と結果を残す計測記録であり、後日のテスト件数や既定値へ遡って書き換えない。

## ディレクトリ

- `../IMPLEMENTATION_STATUS.md`: Git管理外のローカル実装状況、残る改善候補、直近の検証結果
- [`build.md`](build.md): Makefileによるビルド・一括テスト
- [`inc/`](inc/): C++ヘッダーのAPIと利用例
- [`tests/`](tests/): テスト・ベンチマーク・重み調整ツールのビルド方法
- [`server_client/`](server_client/): 回答システムとの取得・送信・自動実行方法
- [`visualizer/`](visualizer.md): GUIでの取得・探索・送信とマップ表示

## 最初に行うこと

リポジトリ直下でソルバーをビルドする。

```sh
make
make test-core
```

公開サンプルを`sample_maps/`へ配置済みなら`make test`でサンプルの回帰テストも実行できる。

ローカルの問題を解くだけなら次のように実行する。

```sh
./main sample_maps/sample_16.json
```

回答システムを使う場合は、Git管理外の`.env`へ接続情報を設定する。詳しくは[`server_client/README.md`](server_client/README.md)を参照する。

## 注意

- コマンド例はすべてリポジトリ直下から実行する。
- JSON生成物、実行ファイル、`.env`、`runtime/`はGit管理対象外である。
- `.env`には本物のトークンが入るため、Gitへ追加しない。
- 回答の送信はGUI、`send_answer.py`、`auto_submit.py`から行える。`main`単体は送信しない。
- 内部処理を共通化した場合も、公開API、生成ファイル、CLI引数に影響する内容は対応するファイル別マニュアルと`IMPLEMENTATION_STATUS.md`へ同時に反映する。
- 競技ルールが変わらない実装改善では`PROBLEM_OVERVIEW.md`を変更せず、アルゴリズム、GUI、テストの対応文書だけを更新する。
