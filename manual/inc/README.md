# C++ヘッダーマニュアル

対象: [`inc/`](../../inc/)

## 基本モデル

- [`types.hpp`](types.md): 基本型、列挙、行動値
- [`cell.hpp`](cell.md): セル、スポット、交通情報
- [`field.hpp`](field.md): 六角盤面と座標変換
- [`agent.hpp`](agent.md): エージェントと行動計画
- [`game.hpp`](game.md): 試合設定と当日状態
- [`game_state.hpp`](game_state.md): 探索・シミュレーション中の状態
- [`path.hpp`](path.md): 経路探索の結果型

## 入出力と実行

- [`json_input.hpp`](json_input.md): 公式JSONの読み込み
- [`create.hpp`](create.md): ランダム問題とJSONの生成
- [`application.hpp`](application.md): CLI補助、回答・チェックポイント出力
- [`solver_options.hpp`](solver_options.md): mainの探索オプション解析
- [`console.hpp`](console.md): 探索状況のコンソール表示
- [`match_runner.hpp`](match_runner.md): オフライン複数日実行

## 探索

- [`brain/brain0.hpp`](brain/brain0.md): ダイクストラ法と燃料制約付き最短路
- [`brain/brain1.hpp`](brain/brain1.md): ビームサーチと最終日の経路方式比較
- [`brand_repair.hpp`](brand_repair.md): 未獲得系列を救済する連鎖再計画
- [`brand_search.hpp`](brand_search.md): 全車の系列最大化と最適性証明
- [`spot_route_lns.hpp`](spot_route_lns.md): スポット単位ビームと複数巡回車の訪問列再編
- [`udon_alns.hpp`](udon_alns.md): 系列を固定して玉数を増やす適応的大規模近傍探索
- [`agent_selector.hpp`](agent_selector.md): 中心補給台数・交通シナリオを使うエージェント種別の自動選択
- [`evaluation.hpp`](evaluation.md): 状態評価
- [`search_state.hpp`](search_state.md): 探索状態の正規化と優越判定
- [`action_routes.hpp`](action_routes.md): マクロ行動列の共有
- [`dial_queue.hpp`](dial_queue.md): 小整数辺用優先度キュー
- [`min_cost_flow.hpp`](min_cost_flow.md): 割当用最小費用流

## 検証と調整

- [`simulator.hpp`](simulator.md): 公式順序での日次シミュレーション
- [`plan_validator.hpp`](plan_validator.md): 回答計画の検証
- [`game_state_hash.hpp`](game_state_hash.md): 状態の完全比較とハッシュ
- [`trial.hpp`](trial.md): 複数日・複数交通条件の試走
- [`weight_tuner.hpp`](weight_tuner.md): 評価重みのオフライン調整
- [`model.hpp`](model.md): 上記モデル・探索APIの一括インクルード
