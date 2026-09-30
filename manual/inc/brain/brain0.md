# `brain/brain0.hpp` の使い方

対象: [`inc/brain/brain0.hpp`](../../../inc/brain/brain0.hpp)

当日の道路状態を反映した最短ステップ経路をダイクストラ法で求める。

## 1地点への経路

```cpp
auto path = benzen::brain0(
    setting,
    state,
    start,
    goal,
    benzen::AgentKind::Patrol);

if (path.reachable) {
    // path.cost.steps、path.cost.fuel、path.directionsを利用
}
```

## 1始点から複数地点

```cpp
auto table = benzen::brain0(setting, state, start);

for (benzen::CellPos goal : goals) {
    if (!table.canReach(goal)) continue;
    auto path = benzen::Brain0::restorePath(setting, table, goal);
}
```

複数の目的地を調べる場合は`DijkstraTable`を一度だけ生成して再利用する。探索の中断判定が必要なら`Brain0::dijkstra()`の第4引数にコールバックを渡せる。

移動ステップは移動元セルと当日の交通状態から計算される。

## 燃料制約付き最短路

```cpp
auto path = benzen::brain0FuelConstrained(
    setting, state, start, goal,
    benzen::AgentKind::Patrol, remainingFuel);
```

`(セル, 使用燃料)`を状態として探索し、指定燃料以内で所要ステップが最小の経路を返す。補給は含まない。従来の`brain0(...)`は燃料制約なしの最短ステップ経路として残している。

燃料制約付き探索では、目的セルに最初に到達した時刻より遅いラベルの処理を打ち切る。同じ到着時刻のラベルは確認し、所要ステップが同じ場合の最小燃料選択を維持する。目的セルから先への展開も省略する。

## 時刻付き補給を含む最短路

```cpp
std::vector<benzen::TimedRefuel> refuels{{meetingCell, meetingStep}};
auto path = benzen::brain0TimedRefuels(
    setting, state, start, goal, benzen::AgentKind::Patrol,
    initialFuel, setting.fuelLimit, 0, stepLimit, refuels);
```

`(位置, step)`で満タンになる補給予定を含めて、目的地への最短到着時刻と経路を返す。
複数ステップ移動の完了までは移動元セルにいるため、その途中に移動元で受ける補給も
反映する。移動予約時に燃料が不足していても、完了前の補給で移動元の燃料消費を
支払える経路は合法として扱う。

## 貪欲ベースライン

```cpp
benzen::DailyPlan plan = benzen::brain0Greedy(setting, state, scoreState);
```

巡回車ごとに未取得系列、当日未取得系列、距離、燃料を基準にスポットを選び、
最短路を順に連結した1日分の合法な計画を生成する。補給車は待機する。
ビームサーチの比較基準や、探索機能を切り分ける回帰テスト向けの単純な実装である。
