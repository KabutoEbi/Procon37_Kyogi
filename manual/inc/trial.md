# `trial.hpp` の使い方

対象: [`inc/trial.hpp`](../../inc/trial.hpp)

エージェント種別や評価重みを比較するため、固定交通条件で複数日をオフライン試走する。

```cpp
auto scenarios = benzen::makeTrialScenarios(setting, 3);

benzen::BeamSearchConfig beam;
beam.beamWidth = 32;
beam.jointBeamWidth = 128;

auto trial = benzen::runTrial(
    setting, kinds, scenarios.front(), beam, 2);

if (trial.success) {
    benzen::TrialScore score = benzen::trialScore(trial.score);
}
```

`TrialTraffic`は`[day][cell]`の道路状態、`TrialScore`は公式優先順の`{試合系列数, 累積日別系列数, 玉数}`である。`dayLimit=0`なら全日を実行する。

`makeTrialScenarios()`は全順調と、決定的に生成した混合交通条件を返す。同じ条件は重複させない。試走中は`MatchState::endsAt`による締切を無効化し、各日の道路状態だけを探索に与える。`BeamSearchConfig::timeLimitMs`を指定した場合の相対時間制限は有効である。
