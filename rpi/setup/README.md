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
