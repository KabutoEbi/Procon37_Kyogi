# `create.hpp` の使い方

対象: [`inc/create.hpp`](../../inc/create.hpp)

ランダム問題、簡易サーバー設定JSON、公式API形式の試合設定・日開始状態を生成する。

## ランダム問題の生成

```cpp
benzen::CreateConfig config;
config.width = 16;
config.height = 16;
config.agentCount = 4;
config.spotCount = 16;
config.brandCount = 12;
config.dayCount = 4;
config.seed = 123;

auto problem = benzen::create(config, "problem.json");
```

同じ`seed`と設定なら同じ問題になる。盤面は8～32、エージェントは3～8、日数は4～10などの制約があり、不正設定は`std::invalid_argument`になる。

この低レベルAPIは小さな人工盤面を使う単体テストにも利用する。ユーザー向けの`create`／`generate`コマンドと`runLocalSolver()`では、さらに[`sample_maps/README.md`](../../sample_maps/README.md)の3区分（16×16・4台・16スポット・10〜14系列、24×24・5台・24スポット・14〜20系列、32×32・7台・32スポット・20〜28系列）と行動回答時間60秒を必須にする。日数など、本選条件が未公開の項目の生成範囲は変更しない。

`ProblemCreator::generateField()`は地形だけ、`generate()`はスポット・開始位置・日設定・チーム情報まで生成する。

## 公式形式JSON

```cpp
benzen::createMatchSettingJson(
    problem.setting, "match-setting.json");

benzen::CreateMatchStateConfig stateConfig;
stateConfig.day = 0;
stateConfig.supplyCount = 1;
auto state = benzen::createMatchStateJson(
    problem.setting, stateConfig, "match-state.json");
```

- `toMatchSettingJson()` / `writeMatchSettingJson()`: `GET /setting`形式
- `generateMatchState()`: テスト用の日開始状態
- `toMatchStateJson()` / `writeMatchStateJson()`: `GET /`形式
- `toJson()` / `writeJson()`: 同梱簡易サーバーの設定形式

ファイル書込関数は成功時に`true`を返す。簡易入口`create()`と状態生成付き`createMatchStateJson()`は書込失敗時に例外を投げる。
