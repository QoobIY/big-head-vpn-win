#!/usr/bin/env bash
set -euo pipefail

# Differential VLESS gRPC/REALITY test. Official Xray is the server and the
# control client; BigHeadVPNProbe is tested against the very same server.
#
# Usage:
#   ./scripts/test-vless-grpc-oracle-wsl.sh /path/to/xray

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

oracle_dir="$(mktemp -d /tmp/bhvpn-grpc-oracle.XXXXXX)"
server_pid=""
control_pid=""
probe_pid=""
origin_pid=""
udp_echo_pid=""
cleanup() {
    [[ -z "$probe_pid" ]] || kill "$probe_pid" 2>/dev/null || true
    [[ -z "$control_pid" ]] || kill "$control_pid" 2>/dev/null || true
    [[ -z "$server_pid" ]] || kill "$server_pid" 2>/dev/null || true
    [[ -z "$origin_pid" ]] || kill "$origin_pid" 2>/dev/null || true
    [[ -z "$udp_echo_pid" ]] || kill "$udp_echo_pid" 2>/dev/null || true
    [[ -z "$probe_pid" ]] || wait "$probe_pid" 2>/dev/null || true
    [[ -z "$control_pid" ]] || wait "$control_pid" 2>/dev/null || true
    [[ -z "$server_pid" ]] || wait "$server_pid" 2>/dev/null || true
    [[ -z "$origin_pid" ]] || wait "$origin_pid" 2>/dev/null || true
    [[ -z "$udp_echo_pid" ]] || wait "$udp_echo_pid" 2>/dev/null || true
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
  "log": {"loglevel": "debug"},
  "inbounds": [{
    "listen": "0.0.0.0", "port": 2444, "protocol": "vless",
    "settings": {
      "clients": [{"id": "00112233-4455-6677-8899-aabbccddeeff"}],
      "decryption": "none"
    },
    "streamSettings": {
      "network": "grpc", "security": "reality",
      "realitySettings": {
        "target": "www.cloudflare.com:443",
        "serverNames": ["www.cloudflare.com"],
        "privateKey": "cAIHpXktoFx357CrnVxKoUR39D1j2TivVV5u80EDyEc",
        "shortIds": ["0123456789abcdef"]
      },
      "grpcSettings": {"serviceName": "bhvpn", "multiMode": false}
    }
  }],
  "outbounds": [{
    "tag": "tcp-origin",
    "protocol": "freedom",
    "settings": {"redirect": "127.0.0.1:28081"}
  }, {
    "tag": "udp-direct",
    "protocol": "freedom"
  }],
  "routing": {
    "rules": [{
      "type": "field", "network": "udp", "outboundTag": "udp-direct"
    }]
  }
}
JSON

cat >"$oracle_dir/control.json" <<JSON
{
  "log": {"loglevel": "warning"},
  "inbounds": [{
    "listen": "0.0.0.0", "port": 2101, "protocol": "socks",
    "settings": {"udp": true}
  }],
  "outbounds": [{
    "protocol": "vless",
    "settings": {"vnext": [{
      "address": "$wsl_address", "port": 2444,
      "users": [{
        "id": "00112233-4455-6677-8899-aabbccddeeff",
        "encryption": "none"
      }]
    }]},
    "streamSettings": {
      "network": "grpc", "security": "reality",
      "realitySettings": {
        "serverName": "www.cloudflare.com", "fingerprint": "chrome",
        "publicKey": "cdll8azvosFOYo1429d1eoZ1_Li7sEXr7_3KNIyNlB0",
        "shortId": "0123456789abcdef"
      },
      "grpcSettings": {"serviceName": "bhvpn", "multiMode": false}
    }
  }]
}
JSON

python3 -m http.server 28081 --bind 127.0.0.1 \
    >"$oracle_dir/origin.log" 2>&1 &
origin_pid=$!
python3 -u -c 'import select, socket
sockets=[]
for port in (28082,28083):
 s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
 s.bind(("0.0.0.0",port))
 sockets.append(s)
while True:
 for s in select.select(sockets,[],[])[0]:
  d,a=s.recvfrom(8192)
  s.sendto(d,a)' >"$oracle_dir/udp-echo.log" 2>&1 &
udp_echo_pid=$!
"$xray_bin" run -c "$oracle_dir/server.json" >"$oracle_dir/server.log" 2>&1 &
server_pid=$!
"$xray_bin" run -c "$oracle_dir/control.json" >"$oracle_dir/control.log" 2>&1 &
control_pid=$!
sleep 1

official_code="$(curl -sS --max-time 15 --proxy socks5h://127.0.0.1:2101 \
    -o /dev/null -w '%{http_code}' http://one.one.one.one/)"
if [[ "$official_code" != "200" ]]; then
    echo "Official Xray gRPC control failed: HTTP $official_code" >&2
    tail -60 "$oracle_dir/control.log" >&2
    tail -80 "$oracle_dir/server.log" >&2
    exit 1
fi

udp_destination="$wsl_address:28082"
if ! "$probe_bin" --socks-udp "$wsl_address" 2101 "$udp_destination" \
    >"$oracle_dir/official-udp.log" 2>&1; then
    echo "Official Xray gRPC UDP control failed" >&2
    tail -100 "$oracle_dir/official-udp.log" >&2
    tail -100 "$oracle_dir/control.log" >&2
    tail -100 "$oracle_dir/server.log" >&2
    exit 1
fi

native_uri="vless://00112233-4455-6677-8899-aabbccddeeff@${wsl_address}:2444?type=grpc&security=reality&serviceName=bhvpn&sni=www.cloudflare.com&pbk=cdll8azvosFOYo1429d1eoZ1_Li7sEXr7_3KNIyNlB0&sid=0123456789abcdef#oracle"
"$probe_bin" --grpc-socks-uri "$native_uri" 2100 20 >"$oracle_dir/probe.log" 2>&1 &
probe_pid=$!
for attempt in {1..15}; do
    grep -q grpc_socks_ready "$oracle_dir/probe.log" && break
    kill -0 "$probe_pid" 2>/dev/null || break
    sleep 1
done
if ! grep -q grpc_socks_ready "$oracle_dir/probe.log"; then
    echo "BigHeadVPN gRPC probe did not start SOCKS5" >&2
    tail -100 "$oracle_dir/probe.log" >&2
    tail -100 "$oracle_dir/server.log" >&2
    exit 1
fi

native_code="$(/mnt/c/Windows/System32/curl.exe -sS --max-time 15 \
    --proxy socks5h://127.0.0.1:2100 -o NUL -w '%{http_code}' \
    http://one.one.one.one/)"
if [[ "$native_code" != "$official_code" ]]; then
    echo "gRPC differential mismatch: official=$official_code native=$native_code" >&2
    tail -100 "$oracle_dir/probe.log" >&2
    tail -100 "$oracle_dir/server.log" >&2
    exit 1
fi

if ! "$probe_bin" --socks-udp 127.0.0.1 2100 "$udp_destination" \
    "$wsl_address:28083" \
    >"$oracle_dir/native-socks-udp.log" 2>&1; then
    echo "BigHeadVPN local SOCKS5 UDP ASSOCIATE failed" >&2
    tail -100 "$oracle_dir/native-socks-udp.log" >&2
    tail -100 "$oracle_dir/probe.log" >&2
    tail -100 "$oracle_dir/server.log" >&2
    exit 1
fi

if ! "$probe_bin" --tunnel-udp-uri "$native_uri" "$udp_destination" \
    >"$oracle_dir/udp-probe.log" 2>&1; then
    echo "BigHeadVPN gRPC UDP probe exited with an error" >&2
    tail -100 "$oracle_dir/udp-probe.log" >&2
    tail -100 "$oracle_dir/server.log" >&2
    exit 1
fi
if ! grep -q 'udp_responses=2' "$oracle_dir/udp-probe.log"; then
    echo "BigHeadVPN gRPC UDP oracle failed" >&2
    tail -100 "$oracle_dir/udp-probe.log" >&2
    tail -100 "$oracle_dir/server.log" >&2
    exit 1
fi

udp_result="$(tail -1 "$oracle_dir/udp-probe.log")"
socks_udp_result="$(tail -1 "$oracle_dir/native-socks-udp.log")"
echo "VLESS gRPC oracle passed: official=$official_code native=$native_code; $socks_udp_result; $udp_result"
