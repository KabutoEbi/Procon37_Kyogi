# `action_routes.hpp` の使い方

対象: [`inc/action_routes.hpp`](../../inc/action_routes.hpp)

同じ残り行動列のsuffixを共有し、マクロ経路のメモリ使用量を抑える内部用コンテナである。

```cpp
procon37::ActionRoutes routes;
procon37::AgentPlan actions = {2, 2, 3};
auto id = routes.intern(actions);

while (id != procon37::ActionRoutes::none) {
    auto entry = routes[id];
    int action = entry.action;
    id = entry.next;
}
```

`intern()`は行動列を登録し、先頭ノードのIDを返す。既存suffixと等しい部分は同じIDを再利用する。`none`は終端を表す。

IDの有効性検査は`operator[]`内では行われないため、このクラスが返したIDだけを使う。通常の利用者は直接触らず、`brain1.hpp`経由で利用する。
