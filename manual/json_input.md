# `json_input.hpp` の使い方

対象: [`inc/json_input.hpp`](../inc/json_input.hpp)

公式の`GET /setting`と`GET /`のJSONを、競技モデルへ読み込む。外部JSONライブラリには依存しない。

## 文字列から読む

```cpp
try {
    auto setting = procon37::parseMatchSettingJson(settingText);
    auto state = procon37::parseMatchStateJson(stateText, setting);
} catch (const procon37::JsonInputError& error) {
    std::cerr << error.what() << '\n';
}
```

## ファイルから読む

```cpp
auto setting =
    procon37::readMatchSettingJson("match-setting.json");
auto state =
    procon37::readMatchStateJson("match-state.json", setting);
auto score =
    procon37::readScoreCheckpointJson("score-checkpoint.json");
```

`MatchState`の解析には、セル数、台数、燃料上限などを照合するため`MatchSetting`が必要である。

構文だけでなく、盤面寸法、日数、開始位置、スポット、道路情報の重複・欠落、値域なども検査する。不正入力ではJSONパスを含む`JsonInputError`が投げられる。

`json_detail`名前空間の`Parser`や`Value`は内部実装用であり、通常は公開簡易関数を使う。
