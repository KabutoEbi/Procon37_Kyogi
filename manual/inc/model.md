# `model.hpp` の使い方

対象: [`inc/model.hpp`](../../inc/model.hpp)

競技モデル、問題生成、JSON入力、探索、検証、複数日実行、表示、重み調整を一括でインクルードする入口である。

```cpp
#include "model.hpp"

int main() {
    procon37::CreateConfig config;
    auto problem = procon37::ProblemCreator::generate(config);

    auto selection =
        procon37::selectAgentKinds(problem.setting);
    auto result = procon37::runMatch(
        problem.setting, selection.kinds);
    return result.success ? 0 : 1;
}
```

注意: `makeInitialState()`などアプリケーション層の関数は`application.hpp`にあり、`model.hpp`には含まれない。モデルと探索APIだけを使う場合は`model.hpp`、CLI補助まで使う場合は`application.hpp`をインクルードする。

ビルド時は`inc`をインクルードパスへ追加する。

```text
c++ -std=c++17 -O2 -pthread -Iinc main.cpp -o solver
```

依存を小さくしたい場合は、一括入口ではなく必要なhppを直接インクルードする。
