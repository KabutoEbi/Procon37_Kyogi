# `sample_maps_test.cpp` の使い方

対象: [`tests/sample_maps_test.cpp`](../../tests/sample_maps_test.cpp)

公開サンプル相当の16、24、32サイズのマップを読み、エージェント選択から1日分の探索・回答検証までをスモークテストする。

## 実行

```sh
c++ -std=c++17 -O2 -pthread -Iinc tests/sample_maps_test.cpp -o /tmp/sample_maps_test
/tmp/sample_maps_test
```

`sample_maps/sample_16.json`、`sample_24.json`、`sample_32.json`が必要である。成功するとマップごとに次を表示する。

```text
sample_maps/sample_16.json agents=... udon=... elapsed-ms=...
```

各マップの探索時間は750 ms、種別選択時間は500 msに制限している。期待する補給車数は16×16が1台、24×24が1台、32×32が2台である。得点の固定値ではなく、中心構成の選択、探索成功、回答のルール適合性を検査するテストである。
