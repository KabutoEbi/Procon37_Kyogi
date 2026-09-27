# `dial_queue.hpp` の使い方

対象: [`inc/dial_queue.hpp`](../../inc/dial_queue.hpp)

非負整数の距離が単調に取り出され、1辺の最大コストが小さい場合に使うDial法のキューである。既定の最大辺コスト4は本問題の道路・平地・山地に対応する。

```cpp
benzen::DialQueue<int> queue;
queue.push(0, start);

while (!queue.empty()) {
    auto [distance, value] = queue.pop();
    // 新距離は distance ～ distance + 4 の範囲でpushする
}
```

`push(distance, value)`は、現在距離以上かつ`current + MaxEdge`以下でなければならない。条件違反はデバッグビルドの`assert`にかかる。一般の優先度付きキューの代替ではなく、上限付き整数辺のダイクストラ専用である。

最大辺コストを変える場合は`DialQueue<T, MaxEdge>`と指定する。
