# `solver_options.hpp` の使い方

対象: [`inc/solver_options.hpp`](../../inc/solver_options.hpp)

`SolverOptions::parse(argc, argv)`は`main`の位置引数を保持したまま探索オプションを読み取る。負数、不正な整数、未知のオプション、0のビーム幅を拒否する。値はスペースで区切って指定する。

```sh
./main solve runtime/setting.json runtime/state-day0.json \
  --beam-width 256 --joint-beam-width 1024 --threads 4 \
  --time-limit-ms 10000 --safety-margin-ms 1000
```

| オプション | 対象 |
|---|---|
| `--beam-width`, `--joint-beam-width` | 日次探索幅 |
| `--threads` | スレッド数。0は自動 |
| `--time-limit-ms` | 日次相対時間上限。0は指定なし |
| `--safety-margin-ms` | 探索・種別選択で残す締切余裕 |
| `--max-actions` | 各車の行動候補数。0は全候補 |
| `--macro-actions`, `--adaptive-beam` | 有効1、無効0 |
| `--trial-candidates` | 種別選択で試走する候補数。0は距離評価のみ |
| `--trial-beam-width`, `--trial-joint-beam-width` | 種別選択の試走幅 |
| `--selection-time-ms` | 種別選択の相対時間上限 |

`selectorFor(setting)`で盤面サイズに応じた既定時間上限と`startsAt`の締切を適用する。種別選択の`minSupplyCount`は既定で1で、時間切れ時のフォールバックにも適用される。日次探索では`endsAt`も引き続き有効。時間オプションを省略した従来CLIの既定値は維持する。GUIは入力値を明示的に渡す。

`positional`は元の`argv`へのポインタを保持するため、呼び出し側はその寿命を維持する。

内部では、整数への変換と範囲検査を`parseValue()`、各設定への反映とオプション固有の検査を`setOption()`が担当する。`parse()`は位置引数とオプションの振り分けだけを行う。この分離は内部実装であり、公開APIとエラーメッセージ、既定値は従来どおりである。
