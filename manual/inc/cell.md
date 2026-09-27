# `cell.hpp` の使い方

対象: [`inc/cell.hpp`](../../inc/cell.hpp)

セル、スポット、道路状態1件を表すデータ型を定義する。

## 主な型

```cpp
benzen::Spot spot{10, 42, 3, -1};
// brand=10, pos=42, maxStock=3

benzen::Cell road{benzen::CellType::Road, -1};
int steps = road.moveSteps(benzen::TrafficStatus::Jammed); // 4
int fuel = road.patrolFuelCost();                            // 2
```

- `Spot::brandIndex`は内部用の連続番号で、`Field::rebuildSpotIndex()`が設定する。
- `Cell::spotIndex`はそのセルにある`Field::spots`の添字。スポットがなければ`-1`。
- `Cell::isPassable()`は池以外で真になる。
- `Cell::moveSteps()`は、そのセルから隣へ出るときの所要ステップを返す。
- `Cell::patrolFuelCost()`は巡回車の移動燃料を返す。補給車の消費0は`game.hpp`側で扱う。
- `TrafficInfo`はAPIの`traffics`要素に対応する。

池に対するコストは`kImpassableCost`になる。通常の有限コストとして加算しないこと。
