# Raspberry Pi AP + KDS server quick setup

These files help configure a Raspberry Pi as the access point + application server
for the Atom Printer based KDS.

## 1. Network configuration

1. Copy `hostapd.conf` to `/etc/hostapd/hostapd.conf` and update `ssid` / `wpa_passphrase` if needed.
2. Copy `dnsmasq.conf` to `/etc/dnsmasq.d/kds.conf` (create the directory if missing).
3. Append the contents of `dhcpcd.conf` to `/etc/dhcpcd.conf` so `wlan0` gets a static IP.
4. Enable IPv4 forwarding:

   ```bash
   echo 'net.ipv4.ip_forward=1' | sudo tee /etc/sysctl.d/99-kds.conf
   sudo sysctl -p /etc/sysctl.d/99-kds.conf
   ```

5. Apply the NAT rules and persist them:

   ```bash
   sudo ./iptables.sh
   ```

## 2. Services

```bash
sudo systemctl restart dhcpcd
sudo systemctl restart dnsmasq
sudo systemctl unmask hostapd
sudo systemctl enable --now hostapd
```

## 3. Application service

1. Copy `.env.example` to `.env` and adjust serial port, baud rate, and optional font path.
2. Install dependencies inside the project virtual environment:

   ```bash
   python -m venv .venv
   source .venv/bin/activate
   pip install -r rpi/requirements.txt
   ```

3. Install the systemd unit:

   ```bash
   sudo cp rpi/atomprinter.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now atomprinter.service
   ```

The FastAPI app exposes the same `/api/*` endpoints and WebSocket contract as the
Atom Lite firmware, so the existing PWA will connect once the Pi's soft AP is running.

## 日本語訳（クイックセットアップ）

以下は上記の英語の手順を日本語に翻訳したものです。Raspberry Pi をソフトアクセスポイントとし、Atom Printer 用 KDS サーバを動かすためのクイックセットアップ手順です。

### 1. ネットワーク設定

1. `hostapd.conf` を `/etc/hostapd/hostapd.conf` にコピーし、必要に応じて `ssid` と `wpa_passphrase` を変更してください。
2. `dnsmasq.conf` を `/etc/dnsmasq.d/kds.conf` にコピーします（ディレクトリが無ければ作成してください）。
3. `dhcpcd.conf` の内容を `/etc/dhcpcd.conf` に追記して、`wlan0` に静的 IP アドレスを割り当てます。
4. IPv4 フォワーディングを有効にします：

```bash
echo 'net.ipv4.ip_forward=1' | sudo tee /etc/sysctl.d/99-kds.conf
sudo sysctl -p /etc/sysctl.d/99-kds.conf
```

5. NAT（マスカレード）ルールを適用し永続化します：

```bash
sudo ./iptables.sh
```

### 2. サービスの再起動

```bash
sudo systemctl restart dhcpcd
sudo systemctl restart dnsmasq
sudo systemctl unmask hostapd
sudo systemctl enable --now hostapd
```

### 3. アプリケーションサービスのセットアップ

1. `.env.example` を `.env` にコピーし、シリアルポート、ボーレート、必要であればフォントのパスを設定してください。
2. プロジェクトの仮想環境を作成して依存パッケージをインストールします：

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r rpi/requirements.txt
```

3. systemd ユニットをインストールして起動します：

```bash
sudo cp rpi/atomprinter.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now atomprinter.service
```

FastAPI アプリは Atom Lite ファームウェアと同じ `/api/*` エンドポイントおよび WebSocket 契約を公開します。Pi のソフト AP が起動すれば、既存の PWA はそのまま接続できます。

---

必要なら、この日本語版を `rpi/README.md` やドキュメントの別ファイルとして統合することもできます。ご希望があれば配置先やフォーマットを調整します。
