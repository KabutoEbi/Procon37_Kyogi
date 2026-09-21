# `brain/brain0.hpp` の使い方

対象: [`inc/brain/brain0.hpp`](../../inc/brain/brain0.hpp)

当日の道路状態を反映した最短ステップ経路をダイクストラ法で求める。

## 1地点への経路

```cpp
auto path = procon37::brain0(
    setting,
    state,
    start,
    goal,
    procon37::AgentKind::Patrol);

if (path.reachable) {
    // path.cost.steps、path.cost.fuel、path.directionsを利用
}
```

## 1始点から複数地点

```cpp
auto table = procon37::brain0(setting, state, start);

for (procon37::CellPos goal : goals) {
    if (!table.canReach(goal)) continue;
    auto path = procon37::Brain0::restorePath(setting, table, goal);
}
```

複数の目的地を調べる場合は`DijkstraTable`を一度だけ生成して再利用する。探索の中断判定が必要なら`Brain0::dijkstra()`の第4引数にコールバックを渡せる。

移動ステップは移動元セルと当日の交通状態から計算される。最短ステップ経路が巡回車の残燃料で通れることまでは保証しない。
