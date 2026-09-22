# `search_state.hpp` の使い方

対象: [`inc/search_state.hpp`](../../inc/search_state.hpp)

ビームサーチで等価状態を統合し、燃料が劣る状態を除去するための補助関数を提供する。

```cpp
auto key = procon37::canonicalSearchState(state);
auto fuelIndependentKey =
    procon37::canonicalSearchState(state, true);

if (procon37::searchStateDominates(a, b)) {
    // bは削除可能
}
```

`canonicalSearchState()`は非アクティブな予約の不要値や、待機行動の表現差を正規化する。`ignoreFuel=true`では巡回車も含めて燃料を0にし、燃料以外の比較キーを作る。

`searchStateDominates(lhs, rhs)`は、位置・時刻・在庫・取得履歴・得点・予約行動などが等価で、かつ全巡回車の燃料が`lhs >= rhs`の場合だけ真になる。
