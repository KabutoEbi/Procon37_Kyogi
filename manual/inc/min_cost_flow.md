# `min_cost_flow.hpp` の使い方

対象: [`inc/min_cost_flow.hpp`](../../inc/min_cost_flow.hpp)

小規模な割当問題用の最小費用流である。`brain1`では巡回車と目標、巡回車と補給車の一対一割当に使う。

```cpp
procon37::MinCostFlow flow(4); // 頂点0～3
int edge = flow.addEdge(0, 1, 1, 5);
flow.addEdge(1, 3, 1, -10);

auto [sent, cost] = flow.run(0, 3, 1);
bool used = flow.capacity(0, edge) == 0;
```

- `addEdge()`は正方向辺の添字を返す。
- `run(source, sink, limit)`は実際に流せた量と総費用を返す。
- `capacity(from, edge)`で実行後の残容量を確認できる。
- `reset(vertices)`で全辺を消去して頂点数を変更する。辺配列の確保領域は再利用するが、以前の辺番号は無効になる。
- `reserveEdges(vertex, count)`で頂点ごとの辺配列を事前確保できる。
- 負の報酬辺には対応するが、初期グラフに負閉路を作ってはならない。

最短増加路の距離・前駆辺・FIFO作業領域はオブジェクト内で再利用する。FIFOは同時に入る頂点数を上限とするリングバッファで、元のSPFAと同じ投入順を保つ。`run()`を続けて呼ぶ場合は残余グラフを引き継ぐ。

探索側は巡回先用・補給車割当用を別々のスレッドローカルオブジェクトとして保持し、毎回`reset()`してから辺を構築する。前の割当の残容量を次の状態へ引き継がない。

頂点番号、辺番号、容量は呼び出し側で有効範囲を保証する。
