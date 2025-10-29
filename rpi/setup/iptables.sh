#!/bin/bash
set -euo pipefail

# Enable NAT from wlan0 (AP) clients out to ethernet uplink
run() {
  if [[ $EUID -ne 0 ]]; then
    sudo "$@"
  else
    "$@"
  fi
}

run iptables -t nat -C POSTROUTING -o eth0 -j MASQUERADE 2>/dev/null || \
  run iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
run iptables -C FORWARD -i eth0 -o wlan0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
  run iptables -A FORWARD -i eth0 -o wlan0 -m state --state RELATED,ESTABLISHED -j ACCEPT
run iptables -C FORWARD -i wlan0 -o eth0 -j ACCEPT 2>/dev/null || \
  run iptables -A FORWARD -i wlan0 -o eth0 -j ACCEPT

# Save rules (requires netfilter-persistent or iptables-persistent)
run sh -c 'iptables-save > /etc/iptables/rules.v4'
