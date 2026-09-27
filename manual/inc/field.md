# `field.hpp` の使い方

対象: [`inc/field.hpp`](../../inc/field.hpp)

六角形盤面、セル列、スポット列、座標変換を管理する。

## 盤面の構築

```cpp
benzen::Field field;
field.width = 8;
field.height = 8;
field.cells.assign(field.cellCount(),
                   {benzen::CellType::Plain, -1});
field.spots.push_back({100, field.toPos({3, 4}), 2, -1});

if (!field.rebuildSpotIndex()) {
    // セル数、スポット位置、地形、上限などが不正
}
```

`spots`を変更した後は必ず`rebuildSpotIndex()`を呼ぶ。これにより`cells[].spotIndex`、`spots[].brandIndex`、`brands`が再構築される。

## 座標と隣接セル

```cpp
using namespace benzen;

CellPos pos = field.toPos({3, 4});
Coord xy = field.toCoord(pos);
CellPos right = field.neighbor(pos, Direction::Right);
auto six = field.neighbors(pos);
```

盤外の変換・隣接先は`kInvalidCell`になる。この盤面は偶数行が右へずれる六角座標系である。`at()`は有効位置を前提としており、事前に`contains()`で確認する。
