# Procon37 solver manual

このディレクトリは、ソルバー本体、GUI、テスト、回答システム用クライアントの使い方を実装ファイル単位でまとめる。初めて使う場合は[ルートREADME](../README.md)から読む。

現行仕様の最終照合日は2026-10-09。系列救済型の連鎖再計画、系列固定の玉数ALNS、`brain2`の焼きなましと複数巡回車・補給車共同destroy/repair、実効時間・盤面サイズ別の長時間探索配分、巡回車別の経路候補組合せ、翌日終端配置、日跨ぎ候補探索、16×16の適応的系列救済、大盤面の終端固定玉数ポートフォリオ、公開試合APIでの比較、5台時の4巡回+1補給を中心とする種別選択、本選3区分に限定したマップ生成、担当1の全自動進行、担当2・3の時間差探索・手動比較送信、口頭3値による次日チェックポイント共有を反映している。日付を含む`tests/*-results-YYYY-MM-DD.md`は、その日時点の条件と結果を残す計測記録であり、後日のテスト件数や既定値へ遡って書き換えない。

## ディレクトリ

- [`../IMPLEMENTATION_STATUS.md`](../IMPLEMENTATION_STATUS.md): 現在の実装状況、残る改善候補、直近の検証結果
- [`build.md`](build.md): Makefileによるビルド・一括テスト
- [`inc/`](inc/): C++ヘッダーのAPIと利用例
- [`tests/`](tests/): テスト・ベンチマーク・重み調整ツールのビルド方法
- [`inc/route_column_search.md`](inc/route_column_search.md): 巡回車別の経路候補生成と在庫制約付き組合せ探索
- [`inc/overnight_refinement.md`](inc/overnight_refinement.md): 翌日の到達性・燃料を保護する終盤再配置
- [`inc/day_beam.md`](inc/day_beam.md): 複数の当日終端候補を将来日の限定探索で比較する実験機能
- [`tests/arena-score-analysis-1671.md`](tests/arena-score-analysis-1671.md): 公開試合1539～1671の得点差と5台時の種別構成分析
- [`tests/arena-score-analysis-1866.md`](tests/arena-score-analysis-1866.md): 公開試合1866までの上位得点傾向と在庫重み付きSA近傍
- [`tests/ahc-inspired-search-2026-10-08.md`](tests/ahc-inspired-search-2026-10-08.md): AHC上位解を参考にした厳燃料終端評価と、不採用案の比較
- [`tests/beam-width-tuning-2026-10-04.md`](tests/beam-width-tuning-2026-10-04.md): ビーム幅の飽和点、自然完了時間、15/35/55秒運用の再測定
- [`tests/adaptive-long-search-2026-10-09.md`](tests/adaptive-long-search-2026-10-09.md): 実効時間・盤面サイズ別のビーム／ALNS配分と比較結果
- [`server_client/`](server_client/): 回答システムとの取得・送信・自動実行方法
- [`visualizer/`](visualizer.md): GUIでの取得・探索・送信とマップ表示
- [`spoken_checkpoint_sharing.md`](spoken_checkpoint_sharing.md): 3整数を口頭で伝えて次日チェックポイントを復元する最短手順

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
