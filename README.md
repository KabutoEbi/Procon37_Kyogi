# ゲシュタルト崩壊、うっっっこれはベンゼン環！？

第37回全国高等専門学校プログラミングコンテスト競技部門「ヘキサうどん」の回答システムをまとめたリポジトリです。

六角形マップ上で巡回車と補給車を動かし、複数日にわたって多くの系列とうどん玉を回収する計画を生成します。C++17製ソルバーを中心に、エージェント種別選択、日次行動探索、公式ルールに沿ったシミュレーション、回答システムとの通信、可視化、回帰テストを収録しています。

## 主な構成

| 場所 | 内容 |
|---|---|
| [`main.cpp`](main.cpp)、[`inc/`](inc/) | エージェント選択、経路探索、得点評価、JSON入出力を行うソルバー |
| [`visualizer/`](visualizer/README.md) | マップ、探索結果、試合進行を扱うGUI |
| [`server_client/`](manual/server_client/README.md) | 回答システムとの取得・送信と自動進行を行うクライアント |
| [`tests/`](manual/tests/README.md) | 回帰テスト、ベンチマーク、探索方式の比較記録 |
| [`manual/`](manual/README.md) | 実装および運用方法の詳細資料 |
| [`sample_maps/`](sample_maps/README.md) | 大会から公開された参考マップの説明 |
| [`docs/`](docs/) | 大会要項、競技仕様、問い合わせ回答などの公式資料 |
| [`match_info/`](match_info/) | 本選マップと試合時の設定、回答、提出履歴、通信ログ |

## 関連資料

- [`PROBLEM_OVERVIEW.md`](PROBLEM_OVERVIEW.md): 競技ルールとAPI仕様の要約
- [`IMPLEMENTATION_STATUS.md`](IMPLEMENTATION_STATUS.md): 実装状況、検証結果、実戦記録
- [`manual/README.md`](manual/README.md): ビルド、実行、試合運用を含む詳細マニュアル
