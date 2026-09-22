# Procon37 solver manual

このディレクトリは、ソルバー本体、テスト、回答システム用クライアントの使い方を実装ファイル単位でまとめる。

## ディレクトリ

- [`inc/`](inc/): C++ヘッダーのAPIと利用例
- [`tests/`](tests/): テスト・ベンチマーク・重み調整ツールのビルド方法
- [`server_client/`](server_client/): 回答システムとの取得・送信・自動実行方法

## 最初に行うこと

リポジトリ直下でソルバーをビルドする。

```sh
c++ -std=c++17 -O2 -pthread main.cpp -o main
```

ローカルの問題を解くだけなら次のように実行する。

```sh
./main sample_maps/sample_16.json
```

回答システムを使う場合は、まず`.env.example`を参考にGit管理外の`.env`へ接続情報を設定する。詳しくは[`server_client/README.md`](server_client/README.md)を参照する。

## 注意

- コマンド例はすべてリポジトリ直下から実行する。
- JSON生成物、実行ファイル、`.env`、`runtime/`はGit管理対象外である。
- `.env`には本物のトークンを保存し、`.env.example`には秘密情報を書かない。
- 実際に回答を送信するのは`send_answer.py`と`auto_submit.py`だけである。
