# `game_state_hash.hpp` の使い方

対象: [`inc/game_state_hash.hpp`](../inc/game_state_hash.hpp)

`GameState`の完全比較とハッシュを提供する。得点、在庫、取得履歴、予約行動、燃料も比較対象に含む。

```cpp
#include "game_state_hash.hpp"
#include <unordered_set>

std::unordered_set<
    procon37::GameState,
    procon37::GameStateHash,
    procon37::GameStateEqual> visited;

visited.insert(state);
bool seen = visited.find(other) != visited.end();
```

`operator==`と`operator!=`も定義されている。将来の挙動が等価な状態を探索用にまとめたい場合は、完全比較ではなく`search_state.hpp`の`canonicalSearchState()`を使う。
