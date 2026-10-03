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
| `--exact-brands` | 1で系列補完の探索件数上限を解除。0は既定の上限付き補完 |
| `--brand-repair` | 系列救済型の連鎖再計画。有効1（既定）、無効0 |
| `--udon-alns` | 系列マスクを固定した玉数ALNS。有効1（既定）、無効0 |
| `--trial-candidates` | 種別選択で試走する候補数。0は距離評価のみ |
| `--trial-beam-width`, `--trial-joint-beam-width` | 種別選択の試走幅 |
| `--selection-time-ms` | 種別選択の相対時間上限 |

`selectorFor(setting)`で盤面サイズに応じた既定時間上限と`startsAt`の締切を適用する。種別選択の`minSupplyCount`は既定で1で、時間切れ時のフォールバックにも適用される。日次探索では`endsAt`も引き続き有効。時間オプションを省略した従来CLIの既定値は維持する。GUIは入力値を明示的に渡す。

`positional`は元の`argv`へのポインタを保持するため、呼び出し側はその寿命を維持する。

最大系列数を証明するまで探索するオフライン実行例（状態JSONの`endsAt`は0）:

```sh
./main solve setting.json state.json --exact-brands 1 --time-limit-ms 0
```

`--exact-brands 1`ではオフラインの暗黙の5秒制限も解除する。探索時間・メモリは問題によって大きくなる。明示的な時間制限や`endsAt`がある場合は締切を守り、未証明ならレポートの`brand-optimal=false`となる。保証の対象は、指定されたエージェント種別・当日交通に対する「当日終了時の試合系列数、当日系列数」の辞書順最大値。将来の交通が未知の試合全体や種別選択の最適性、獲得玉数の最適性は含まない。

内部では、整数への変換と範囲検査を`parseValue()`、各設定への反映とオプション固有の検査を`setOption()`が担当する。`parse()`は位置引数とオプションの振り分けだけを行う。この分離は内部実装であり、公開APIとエラーメッセージ、既定値は従来どおりである。
