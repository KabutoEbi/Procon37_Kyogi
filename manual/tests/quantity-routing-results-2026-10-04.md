# 玉数低下を抑える経路・探索・合流改善（2026-10-04）

## 変更内容

- 経路決定: 2店目の最良候補に加えて次善候補を1/16の強さで評価し、他車との在庫競合後にも続行可能な巡回先を選びやすくした。
- 探索: ビームの多様性署名へ巡回車ごとの取得済みスポットを追加し、同系列の別店舗を回った候補を残した。
- 合流: 一対一最小費用流で主担当を決めた後、同じ補給車・同じセルへの経路を共有できる巡回車を追加した。
- 短時間保護: 実効探索予算が500ms未満なら上記を無効化し、既存探索の深さを維持する。

## 固定探索量・4日間

8/16/24/32の4サイズ、seed 21〜24、各4日、混合交通、ビーム幅32、同時行動幅128、1スレッドで比較した。

| 構成 | 試合系列 | 日別系列 | 玉数 |
|---|---:|---:|---:|
| 変更前 (`no-quantity-upgrades`) | 240 | 959 | 2924 |
| 改善後 (`improved`) | 240 | 959 | 3191 |

玉数は267増加（約9.1%）し、系列指標は維持した。

## 別条件

| 条件 | 変更前 | 改善後 |
|---|---:|---:|
| seed 1〜8、初日のみ、混合交通、固定探索量（32試合） | 479 / 479 / 1761 | 479 / 479 / 1761 |
| seed 41〜44、4日、混合交通、燃料半減、固定探索量（16試合） | 240 / 909 / 1950 | 240 / 913 / 2009 |

数値は試合系列／日別系列／玉数。全計画を公式シミュレータで再実行し、合法性と探索終了状態の一致を確認した。

300ms条件では取得スポット多様化が探索深度を奪うケースがあったため、既定では実効予算500ms未満で3改善を無効化する。競技用の長い回答時間と、時間制限なしの固定探索量では有効になる。

## 再現

```sh
make .build/tools/solver_benchmark

.build/tools/solver_benchmark --current-only --variant no-quantity-upgrades \
  --seed-start 21 --seeds 4 --repeats 1 --days 4 --fixed-work --mixed-traffic
.build/tools/solver_benchmark --current-only --variant improved \
  --seed-start 21 --seeds 4 --repeats 1 --days 4 --fixed-work --mixed-traffic

.build/tools/solver_benchmark --current-only --variant no-quantity-upgrades \
  --seed-start 41 --seeds 4 --repeats 1 --days 4 --fixed-work --mixed-traffic --tight-fuel
.build/tools/solver_benchmark --current-only --variant improved \
  --seed-start 41 --seeds 4 --repeats 1 --days 4 --fixed-work --mixed-traffic --tight-fuel
```
