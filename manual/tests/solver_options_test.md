# `solver_options_test.cpp` の使い方

対象: [`tests/solver_options_test.cpp`](../../tests/solver_options_test.cpp)

GUIから渡すビーム幅、スレッド数、時間制限、マクロ設定などの反映と不正値の拒否を検査する。

```sh
c++ -std=c++17 -O2 -pthread -Iinc tests/solver_options_test.cpp -o /tmp/solver_options_test
/tmp/solver_options_test
```

`assert`で検査するため`-DNDEBUG`を指定しない。
