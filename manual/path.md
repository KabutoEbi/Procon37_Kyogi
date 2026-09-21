# `path.hpp` の使い方

対象: [`inc/path.hpp`](../inc/path.hpp)

最短経路探索の入出力データ型を定義する。

## 主な型

- `RouteCost`: 所要ステップと必要燃料
- `PathResult`: 到達可否、始点・終点、コスト、セル列、方向列
- `DijkstraTable`: 1始点から全セルへの距離、前駆、最初の移動先
- `FuelPathState`, `FuelPathLabel`: 燃料を状態に含む経路探索用ラベル

```cpp
auto table = procon37::brain0(setting, state, start);
if (table.canReach(goal)) {
    auto path = procon37::Brain0::restorePath(
        setting, table, goal, procon37::AgentKind::Patrol);
    // path.positions.front() == start
    // path.positions.back() == goal
}
```

`positions`は始点と終点を含み、`directions.size() + 1 == positions.size()`となる。補給車として復元すると`RouteCost::fuel`は0になる。
