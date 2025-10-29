# rpi: Raspberry Pi 用サーバー

このディレクトリには Atom Printer + Atom Lite で動作していた KDS を Raspberry Pi 上で再現するための
FastAPI 実装とセットアップ用ファイルが含まれます。

## 含まれるもの

- `app.py` – REST / WebSocket / 印刷キューを提供する FastAPI アプリ
- `printer.py` – シリアルプリンタへの ESC/POS 出力アダプタ
- `data/www/` – KDS フロントエンド PWA（ESP32 版と同一構成）
- `store.py` – スナップショット + WAL による永続化レイヤー
- `setup/` – hostapd / dnsmasq / iptables のサンプル設定
- `.env.example` – サービス起動用の環境変数テンプレート

## セットアップ手順

```bash
cd /home/pi/atomprinter
python -m venv .venv
source .venv/bin/activate
pip install -r rpi/requirements.txt
cp rpi/.env.example .env
# シリアルデバイスやフォントパスが異なる場合は .env を編集
```

### ソフト AP (hostapd + dnsmasq)

`rpi/setup/README.md` に、`wlan0` を固定 IP (例: `192.168.50.1/24`) に設定し、NAT を有効化する手順をまとめています。
以下のファイルをベースに `/etc/` 配下へコピーしてください。

- `hostapd.conf` → `/etc/hostapd/hostapd.conf`
- `dnsmasq.conf` → `/etc/dnsmasq.d/kds.conf`
- `dhcpcd.conf` の内容 → `/etc/dhcpcd.conf` へ追記
- `iptables.sh` → NAT 設定スクリプト（実行後に永続化）

### systemd サービス

```bash
sudo cp rpi/atomprinter.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now atomprinter.service
```

サービスは `/home/pi/atomprinter/.venv/bin/uvicorn` を想定しています。仮想環境の場所を変える場合は
`atomprinter.service` を編集してください。

### 手動起動 (デバッグ)

```bash
source .venv/bin/activate
python -m uvicorn rpi.app:app --host 0.0.0.0 --port 8000
```

## 動作仕様のポイント

- `/api/*` エンドポイントおよび WebSocket イベントは Atom Lite (ESP32) 版と互換です。
- 受注・メニュー・売上サマリは `rpi/storage/` に `snapshot.json` + `wal.log` として保存されます。
- 印刷キューはバックグラウンドタスクで処理され、失敗時はテキストフォールバックを自動実行します。
- `FONT_PATH` を設定すると日本語対応フォントでレシート画像を描画できます。未設定の場合は ASCII のみのテキスト印字です。
- 実運用ではログローテーション・バックアップ・監視などを環境に合わせて整備してください。
