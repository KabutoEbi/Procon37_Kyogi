# `types.hpp` の使い方

対象: [`inc/types.hpp`](../inc/types.hpp)

競技全体で使う基本型、定数、列挙型を定義する最下層のhppである。

## 主な定義

- `CellPos`: `pos = y * width + x` で表すセル番号
- `BrandId`, `PlayerId`, `AgentIndex`: 系列、チーム、エージェントの識別型
- `kInvalidCell`: 無効なセル番号 `-1`
- `kMaxAgents`, `kMaxSpots`, `kMaxBrands`: 固定長探索状態の上限
- `CellType`: 平地、道路、山、池
- `TrafficStatus`: 順調、混雑、渋滞
- `AgentKind`: 巡回車、補給車
- `Direction`: 行動値0～5に対応する六方向
- `Coord`: `(x, y)` 座標

## 行動値

```cpp
using namespace procon37;

const int move = toAction(Direction::Right); // 2
const bool a = isMoveAction(move);           // true
const bool b = isWaitAction(-15);            // true
const int wait = waitSteps(-15);             // 15
```

移動は`0`～`5`、待機は負数で表す。`-1`は1ステップ、`-15`は15ステップ待機である。`0`は待機ではなく左上への移動なので注意する。
