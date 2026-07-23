#!/usr/bin/env bash
set -euo pipefail

# Differential end-to-end test:
#   official Xray client -> official Xray server -> HTTP
#   BigHeadVPN client    -> official Xray server -> HTTP
#
# Usage:
#   ./scripts/test-vless-vision-oracle-wsl.sh /path/to/xray
#
# The Xray binary is a test oracle only. It is not copied into the application.

xray_bin="${1:-}"
probe_bin="${2:-./build/native-windows/BigHeadVPNProbe.exe}"
if [[ ! -x "$xray_bin" ]]; then
    echo "Usage: $0 /path/to/official-xray [path/to/BigHeadVPNProbe.exe]" >&2
    exit 2
fi
if [[ ! -f "$probe_bin" ]]; then
    echo "Probe not found: $probe_bin" >&2
    exit 2
fi

oracle_dir="$(mktemp -d /tmp/bhvpn-vision-oracle.XXXXXX)"
server_pid=""
control_pid=""
probe_pid=""
origin_pid=""
cleanup() {
    [[ -z "$probe_pid" ]] || kill "$probe_pid" 2>/dev/null || true
    [[ -z "$control_pid" ]] || kill "$control_pid" 2>/dev/null || true
    [[ -z "$server_pid" ]] || kill "$server_pid" 2>/dev/null || true
    [[ -z "$origin_pid" ]] || kill "$origin_pid" 2>/dev/null || true
    [[ -z "$probe_pid" ]] || wait "$probe_pid" 2>/dev/null || true
    [[ -z "$control_pid" ]] || wait "$control_pid" 2>/dev/null || true
    [[ -z "$server_pid" ]] || wait "$server_pid" 2>/dev/null || true
    [[ -z "$origin_pid" ]] || wait "$origin_pid" 2>/dev/null || true
    rm -rf -- "$oracle_dir"
}
trap cleanup EXIT

wsl_address="$(hostname -I | awk '{print $1}')"
if [[ -z "$wsl_address" ]]; then
    echo "Could not determine the WSL address" >&2
    exit 1
fi

cat >"$oracle_dir/server.json" <<'JSON'
{
  "log": {"loglevel": "warning"},
  "inbounds": [{
    "listen": "0.0.0.0", "port": 2443, "protocol": "vless",
    "settings": {
      "clients": [{
        "id": "00112233-4455-6677-8899-aabbccddeeff",
        "flow": "xtls-rprx-vision"
      }],
      "decryption": "none"
    },
    "streamSettings": {
      "network": "raw", "security": "reality",
      "realitySettings": {
        "target": "www.cloudflare.com:443",
        "serverNames": ["www.cloudflare.com"],
        "privateKey": "cAIHpXktoFx357CrnVxKoUR39D1j2TivVV5u80EDyEc",
        "shortIds": ["0123456789abcdef"]
      }
    }
  }],
  "outbounds": [{
    "protocol": "freedom",
    "settings": {"redirect": "127.0.0.1:28080"}
  }]
}
JSON

cat >"$oracle_dir/control.json" <<JSON
{
  "log": {"loglevel": "warning"},
  "inbounds": [{
    "listen": "127.0.0.1", "port": 2099, "protocol": "socks",
    "settings": {"udp": false}
  }],
  "outbounds": [{
    "protocol": "vless",
    "settings": {"vnext": [{
      "address": "$wsl_address", "port": 2443,
      "users": [{
        "id": "00112233-4455-6677-8899-aabbccddeeff",
        "encryption": "none", "flow": "xtls-rprx-vision"
      }]
    }]},
    "streamSettings": {
      "network": "raw", "security": "reality",
      "realitySettings": {
        "serverName": "www.cloudflare.com", "fingerprint": "chrome",
        "publicKey": "cdll8azvosFOYo1429d1eoZ1_Li7sEXr7_3KNIyNlB0",
        "shortId": "0123456789abcdef"
      }
    }
  }]
}
JSON

python3 -m http.server 28080 --bind 127.0.0.1 \
    >"$oracle_dir/origin.log" 2>&1 &
origin_pid=$!
"$xray_bin" run -c "$oracle_dir/server.json" >"$oracle_dir/server.log" 2>&1 &
server_pid=$!
"$xray_bin" run -c "$oracle_dir/control.json" >"$oracle_dir/control.log" 2>&1 &
control_pid=$!
sleep 1

official_code="$(curl -sS --max-time 15 --proxy socks5h://127.0.0.1:2099 \
    -o /dev/null -w '%{http_code}' http://one.one.one.one/)"
if [[ "$official_code" != "200" ]]; then
    echo "Official Xray control failed: HTTP $official_code" >&2
    tail -40 "$oracle_dir/control.log" >&2
    tail -40 "$oracle_dir/server.log" >&2
    exit 1
fi

native_uri="vless://00112233-4455-6677-8899-aabbccddeeff@${wsl_address}:2443?type=tcp&security=reality&flow=xtls-rprx-vision&sni=www.cloudflare.com&pbk=cdll8azvosFOYo1429d1eoZ1_Li7sEXr7_3KNIyNlB0&sid=0123456789abcdef#oracle"
"$probe_bin" --socks-uri "$native_uri" 2098 >"$oracle_dir/probe.log" 2>&1 &
probe_pid=$!
for attempt in {1..15}; do
    grep -q socks_ready "$oracle_dir/probe.log" && break
    kill -0 "$probe_pid" 2>/dev/null || break
    sleep 1
done
if ! grep -q socks_ready "$oracle_dir/probe.log"; then
    echo "BigHeadVPN probe did not start SOCKS5" >&2
    tail -60 "$oracle_dir/probe.log" >&2
    tail -60 "$oracle_dir/server.log" >&2
    exit 1
fi

native_code="$(/mnt/c/Windows/System32/curl.exe -sS --max-time 15 \
    --proxy socks5h://127.0.0.1:2098 -o NUL -w '%{http_code}' \
    http://one.one.one.one/)"
if [[ "$native_code" != "$official_code" ]]; then
    echo "Differential mismatch: official=$official_code native=$native_code" >&2
    tail -60 "$oracle_dir/probe.log" >&2
    tail -60 "$oracle_dir/server.log" >&2
    exit 1
fi

echo "VLESS Vision oracle passed: official=$official_code native=$native_code"
