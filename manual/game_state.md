# `game_state.hpp` の使い方

対象: [`inc/game_state.hpp`](../inc/game_state.hpp)

1日分の探索とシミュレーションで変化する状態を固定長配列で保持する。

## 初期化

```cpp
procon37::GameState state;
procon37::ScoreState previous;

if (!state.initialize(setting, matchState, previous)) {
    // 日番号、台数、位置、スポットなどが不正
}
```

主な状態は現在ステップ、エージェント、予約行動、在庫、当日取得済みスポット、当日系列、累積得点である。

## 得点とビット集合

```cpp
auto spot = procon37::spotBit(3);
auto brand = procon37::brandBit(2);
auto count = procon37::countBits(brand);
```

`ScoreState`の`matchBrands`、`totalUdon`、`sumOfDailyBrandCounts`は日をまたいで引き継ぐ。`finishDay()`は最終ステップ到達後に一度だけ呼び、当日の系列数を累積値へ確定する。

`ScoreCheckpoint`は`nextDay`と`ScoreState`をまとめ、再起動後にどの日の開始状態へ適用できるかを検査できるようにする。

通常は`tryAcquire()`、`refuelCoLocated()`、`advanceTo()`を直接組み合わせず、ルール順序を保証する`simulator.hpp`を使う。
