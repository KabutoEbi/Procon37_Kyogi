# `agent_selector.hpp` の使い方

対象: [`inc/agent_selector.hpp`](../../inc/agent_selector.hpp)

試合開始前に、各エージェントを巡回車または補給車へ自動割当する。

```cpp
benzen::AgentSelectorConfig config;
config.minSupplyCount = 1;
config.maxSupplyCount = 2;
config.trialCandidateLimit = 12;
config.trialDays = 0; // 全日試走

auto selected = benzen::selectAgentKinds(setting, config);
if (selected.success) {
    const std::vector<benzen::AgentKind>& kinds = selected.kinds;
    // kindsを試合開始時のPOST /agentやrunMatch()へ渡す
}
```

最初に全配置を到達範囲、各巡回車が回れる近いスポット、補給距離で評価する。補給車の台数ごとの最良配置と、補給車を離して置く配置を確保し、残りの候補枠は「補給車1台で巡回車およそ2台」となる台数と、その1台少ない構成へ重点配分する。8台では補給2台・3台の位置違いを交互に追加する。これらを`brain1`で複数日・複数交通シナリオ試走する。既定の試走候補数は12である。既定の交通シナリオは設定のチーム数・エージェント数と道路閾値から予測した低位・中央・高位条件である。候補はシナリオの合計スコア、次に最悪スコアで比較する。既定では全日を試走し、補給車を最低1台、巡回車を補給車以上残す。したがって3台の場合は巡回車2台・補給車1台になる。

## 主な設定

- `minSupplyCount`, `maxSupplyCount`: 補給車台数の範囲。既定の最小は1、最大`-1`では全台数の半分（切り捨て）が自動上限。明示値では半数超も指定可能
- `trialCandidateLimit`: 試走する候補数。0なら距離評価だけ
- `trialBeamWidth`, `trialJointBeamWidth`: 試走用探索幅
- `trialDays`: 試走日数。0なら全日
- `trialScenarioCount`: 予測交通量から自動生成するシナリオ数
- `trialScenarios`: 明示的な交通条件
- `timeLimitMs`: 種別選択全体の相対時間制限
- `deadlineUnixSeconds`: 明示的に分かっている種別締切のUNIX時刻
- `safetyMarginMs`: 締切前に残す余裕

結果には選択種別、補給車数、評価候補数、試走回数、最悪・合計試走スコア、`timedOut`が入る。時間切れでも、距離評価済みの最良構成または設定された最低補給車数を満たす構成を返す。設定不整合は`std::invalid_argument`になる。

短い締切でも台数比較を失わないよう、最初に許可範囲の各補給車台数から最良配置を1件ずつ試す。続いて各台数の広域配置、中心となる補給台数の位置違いを試す。距離表を再利用して複数日・複数シナリオの試走を進め、試走全体の残り時間は各日の開始時に再配分する。

同じ候補の交通シナリオでは、初日から道路状態が連続して一致する日まで、直前の試走結果を再利用する。交通条件が分岐した日以降は個別に探索する。各開始位置の近い3スポットによる巡回能力は、全配置で共通なので1回だけ計算する。

`completedTrialScenarios`は選択に採用した候補が完走したシナリオ数である。全指定シナリオを完走した候補だけを比較し、未完走候補の部分的な得点は採用しない。`trialAssignments`と`trialSimulations`は着手件数なので、完走数とは異なる。日次探索は時間内の最良有効解を使用し、「試走完走」は指定日数のシミュレーション完了を意味する。

交通予測は試合開始前に分かる集計値だけを使う。エージェント数が多いほど予測平均を高くし、チーム数が多いほどチーム平均の予測幅を狭くする。実際の他チームの種別・目的地・経路は不明なので、道路ごとの結果は確定値ではない。試合開始後の日次探索では予測を使わず、APIから配布された当日の道路状態を使う。
