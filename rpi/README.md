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

3. 環境変数ファイル `.env` をプロジェクトルートに置く（`rpi/setup/.env.example` を参照）
4. systemd か手動で起動

手動起動の例（テスト用）:

```powershell
# 仮想環境内から
python -m uvicorn rpi.app:app --host 0.0.0.0 --port 8000
```

注: サンプル実装は最小限です。印刷内容のフォーマット（ESC/POS ラスター印刷など）は ESP32 側の実装に合わせて移植してください。
