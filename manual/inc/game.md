# `game.hpp` の使い方

対象: [`inc/game.hpp`](../../inc/game.hpp)

試合開始前の設定`MatchSetting`と、各日開始時の情報`MatchState`を定義する。

## 主な型

- `MatchSetting`: 日数・制限時間・盤面・初期位置・燃料上限・参加チーム数・交通閾値
- `MatchState`: 当日番号、締切時刻、自他チームの現在位置と燃料、当日の道路状態

```cpp
// json_input.hppを併用する例
benzen::MatchSetting setting =
    benzen::readMatchSettingJson("match-setting.json");
benzen::MatchState state =
    benzen::readMatchStateJson("match-state.json", setting);

auto days = setting.dayCount();
auto agents = setting.agentCount();
auto traffic = state.trafficAt(10);
```

## コスト取得

```cpp
int steps = benzen::moveStepCost(setting, state, from);
int fuel = benzen::moveFuelCost(
    setting, benzen::AgentKind::Patrol, from);
```

どちらも「移動元セル」のコストを返す。道路のステップ数だけが`MatchState::trafficByPos`の影響を受ける。補給車の燃料コストは常に0である。

`makeTrafficTable()`はAPI形式の疎な`TrafficInfo`列を、セル番号でO(1)参照できる配列へ変換する。

`MatchState::others`には他チームの状態も保持できるが、現在の`brain0`と`brain1`は自チームと当日の`trafficByPos`だけを探索に使用する。
