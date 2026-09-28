# `agent_selector.hpp` の使い方

対象: [`inc/agent_selector.hpp`](../../inc/agent_selector.hpp)

試合開始前に、各エージェントを巡回車または補給車へ自動割当する。

```cpp
benzen::AgentSelectorConfig config;
config.minSupplyCount = 1;
config.maxSupplyCount = 2;
config.trialCandidateLimit = 8;
config.trialDays = 0; // 全日試走

auto selected = benzen::selectAgentKinds(setting, config);
if (selected.success) {
    const std::vector<benzen::AgentKind>& kinds = selected.kinds;
    // kindsを試合開始時のPOST /agentやrunMatch()へ渡す
}
```

最初に全配置を到達範囲、各巡回車が回れる近いスポット、補給距離で評価する。補給車の台数ごとの上位配置に加え、最小補給台数で位置を変えた次善案と、補給車を離して置く配置も候補に入れ、`brain1`で複数日・複数交通シナリオ試走する。候補はシナリオの合計スコア、次に最悪スコアで比較する。既定では全日を試走し、補給車を最低1台、巡回車を最低1台残す。

## 主な設定

- `minSupplyCount`, `maxSupplyCount`: 補給車台数の範囲。既定の最小は1、最大`-1`で自動上限
- `trialCandidateLimit`: 試走する候補数。0なら距離評価だけ
- `trialBeamWidth`, `trialJointBeamWidth`: 試走用探索幅
- `trialDays`: 試走日数。0なら全日
- `trialScenarioCount`: 自動生成する交通シナリオ数
- `trialScenarios`: 明示的な交通条件
- `timeLimitMs`: 種別選択全体の相対時間制限
- `deadlineUnixSeconds`: 明示的に分かっている種別締切のUNIX時刻
- `safetyMarginMs`: 締切前に残す余裕

結果には選択種別、補給車数、評価候補数、試走回数、最悪・合計試走スコア、`timedOut`が入る。時間切れでも、距離評価済みの最良構成または設定された最低補給車数を満たす構成を返す。設定不整合は`std::invalid_argument`になる。
