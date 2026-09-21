# `agent_selector.hpp` の使い方

対象: [`inc/agent_selector.hpp`](../inc/agent_selector.hpp)

試合開始前に、各エージェントを巡回車または補給車へ自動割当する。

```cpp
procon37::AgentSelectorConfig config;
config.minSupplyCount = 0;
config.maxSupplyCount = 2;
config.trialCandidateLimit = 8;
config.trialDays = 2;

auto selected = procon37::selectAgentKinds(setting, config);
if (selected.success) {
    const std::vector<procon37::AgentKind>& kinds = selected.kinds;
    // kindsを試合開始時のPOST /agentやrunMatch()へ渡す
}
```

最初に全配置を距離ヒューリスティックで評価し、上位候補を`brain1`で複数日・複数交通シナリオ試走する。最低でも1台は巡回車として残す。

## 主な設定

- `minSupplyCount`, `maxSupplyCount`: 補給車台数の範囲。最大`-1`で自動上限
- `trialCandidateLimit`: 試走する候補数。0なら距離評価だけ
- `trialBeamWidth`, `trialJointBeamWidth`: 試走用探索幅
- `trialDays`: 試走日数。0なら全日
- `trialScenarioCount`: 自動生成する交通シナリオ数
- `trialScenarios`: 明示的な交通条件

結果には選択種別、補給車数、評価候補数、試走回数、最悪・合計試走スコアが入る。設定不整合は`std::invalid_argument`になる。
