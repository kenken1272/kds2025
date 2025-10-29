# rpi: Raspberry Pi 用 サーバー (最小実装)

このフォルダには Raspberry Pi 上で動かすための最小限の FastAPI サーバー実装が含まれます。

含まれるファイル:
- `requirements.txt` - 必要パッケージ
- `app.py` - FastAPI アプリ (静的配信 + /api + /ws + バックグラウンド印刷ワーカー)
- `printer.py` - pyserial を使う簡易プリンタアダプタ

使い方(ラズパイ側):

1. リポジトリを /home/pi/atomprinter に配置
2. Python 仮想環境を作成・アクティベート

```powershell
python -m venv .venv
.venv\Scripts\Activate.ps1
pip install -r rpi/requirements.txt
```

## systemd と AP の簡単手順（概要）

1. `.env` を作成（下の `.env.example` を参照）
2. `rpi/atomprinter.service` を `/etc/systemd/system/atomprinter.service` にコピーしてパス・ユーザーを確認
3. hostapd/dnsmasq の設定は本 README の後半を参照（手動での設定は環境に依存します）
4. systemd をリロードして有効化

```powershell
sudo systemctl daemon-reload
sudo systemctl enable atomprinter.service
sudo systemctl start atomprinter.service
```

## .env の例
設定の最小例は `rpi/.env.example` を参照してください。主な項目:
- TTY_DEVICE: /dev/ttyUSB0 など実機の接続先
- BAUD: 115200
- API_PORT: 8000

## 注意
この実装は「ラズパイ上で素早く動作確認できる」ことを目的にしており、実運用では hostapd/dnsmasq の設定、systemd ユニットのユーザーや仮想環境パス、ログローテーションなどを適切に調整してください。

3. 環境変数ファイル `.env` をプロジェクトルートに置く（`rpi/setup/.env.example` を参照）
4. systemd か手動で起動

手動起動の例（テスト用）:

```powershell
# 仮想環境内から
python -m uvicorn rpi.app:app --host 0.0.0.0 --port 8000
```

注: サンプル実装は最小限です。印刷内容のフォーマット（ESC/POS ラスター印刷など）は ESP32 側の実装に合わせて移植してください。
