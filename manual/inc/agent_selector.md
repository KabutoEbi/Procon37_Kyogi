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

最初に全配置を到達範囲、各巡回車が回れる近いスポット、補給距離で評価する。補給車の台数ごとの上位配置に加え、最小補給台数で位置を変えた次善案と、補給車を離して置く配置も候補に入れ、`brain1`で複数日・複数交通シナリオ試走する。既定の試走候補数は12で、従来の先頭8候補を維持しながら時間に余裕がある場合に次点4候補も比較する。既定の交通シナリオは設定のチーム数・エージェント数と道路閾値から予測した低位・中央・高位条件である。候補はシナリオの合計スコア、次に最悪スコアで比較する。既定では全日を試走し、補給車を最低1台、巡回車を補給車以上残す。したがって3台の場合は巡回車2台・補給車1台になる。

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

短い締切でも配置の違いを検討できるよう、最初の補給車台数2通りの比較に続いて、最小補給台数の別配置を試す。多数の補給車を置く候補より、この位置違いの候補を先に評価する。距離表を再利用して複数日・複数シナリオの試走を進め、試走全体の残り時間は各日の開始時に再配分する。

同じ候補の交通シナリオで初日の道路状態が同じ場合、初日の探索と取得結果を1回だけ計算して使い回す。2日目以降は各シナリオの道路状態で個別に探索する。道路状態が初日から異なる明示シナリオには流用しない。

`completedTrialScenarios`は選択に採用した候補が完走したシナリオ数である。全指定シナリオを完走した候補だけを比較し、未完走候補の部分的な得点は採用しない。`trialAssignments`と`trialSimulations`は着手件数なので、完走数とは異なる。日次探索は時間内の最良有効解を使用し、「試走完走」は指定日数のシミュレーション完了を意味する。

交通予測は試合開始前に分かる集計値だけを使う。エージェント数が多いほど予測平均を高くし、チーム数が多いほどチーム平均の予測幅を狭くする。実際の他チームの種別・目的地・経路は不明なので、道路ごとの結果は確定値ではない。試合開始後の日次探索では予測を使わず、APIから配布された当日の道路状態を使う。
