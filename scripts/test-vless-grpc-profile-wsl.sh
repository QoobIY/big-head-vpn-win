#!/usr/bin/env bash
set -euo pipefail

# Differential test of one real VLESS gRPC/REALITY subscription profile.
#
# Usage:
#   ./scripts/test-vless-grpc-profile-wsl.sh \
#       /path/to/xray subscription.txt grpc-profile-number

xray_bin="${1:-}"
subscription_file="${2:-}"
profile_number="${3:-}"
probe_bin="${4:-./build/native-windows/BigHeadVPNProbe.exe}"
if [[ ! -x "$xray_bin" || ! -f "$subscription_file" ||
      ! "$profile_number" =~ ^[1-9][0-9]*$ || ! -f "$probe_bin" ]]; then
    echo "Usage: $0 /path/to/xray subscription.txt grpc-profile-number [probe.exe]" >&2
    exit 2
fi

profile_uri="$(awk -v wanted="$profile_number" \
    '/^vless:\/\// && /type=grpc/ {++seen; if (seen == wanted) {print; exit}}' \
    "$subscription_file")"
if [[ -z "$profile_uri" ]]; then
    echo "gRPC profile $profile_number was not found" >&2
    exit 2
fi

without_scheme="${profile_uri#vless://}"
uuid="${without_scheme%%@*}"
server_and_query="${without_scheme#*@}"
endpoint="${server_and_query%%\?*}"
host="${endpoint%:*}"
port="${endpoint##*:}"
query="${server_and_query#*\?}"
query="${query%%#*}"

query_value() {
    local wanted="$1" field
    local IFS='&'
    for field in $query; do
        if [[ "${field%%=*}" == "$wanted" ]]; then
            printf '%s' "${field#*=}"
            return
        fi
    done
}

sni="$(query_value sni)"
public_key="$(query_value pbk)"
if [[ -z "$public_key" ]]; then public_key="$(query_value password)"; fi
short_id="$(query_value sid)"
service_name="$(query_value serviceName)"
authority="$(query_value authority)"

[[ "$uuid" =~ ^[0-9A-Fa-f-]{36}$ ]] ||
    { echo "Invalid VLESS UUID" >&2; exit 2; }
[[ "$host" =~ ^[0-9A-Za-z.-]+$ && "$port" =~ ^[0-9]{1,5}$ ]] ||
    { echo "Invalid VLESS endpoint" >&2; exit 2; }
[[ "$sni" =~ ^[0-9A-Za-z.-]+$ ]] ||
    { echo "Invalid REALITY SNI" >&2; exit 2; }
[[ "$public_key" =~ ^[0-9A-Za-z_-]+$ && "$short_id" =~ ^[0-9A-Fa-f]*$ ]] ||
    { echo "Invalid REALITY credentials" >&2; exit 2; }
[[ "$service_name" =~ ^[0-9A-Za-z._~/-]*$ ]] ||
    { echo "Invalid gRPC service name" >&2; exit 2; }
[[ -z "$authority" || "$authority" =~ ^[0-9A-Za-z.:-]+$ ]] ||
    { echo "Invalid gRPC authority" >&2; exit 2; }

test_dir="$(mktemp -d /tmp/bhvpn-grpc-profile.XXXXXX)"
xray_pid=""
probe_pid=""
cleanup() {
    [[ -z "$probe_pid" ]] || kill "$probe_pid" 2>/dev/null || true
    [[ -z "$xray_pid" ]] || kill "$xray_pid" 2>/dev/null || true
    [[ -z "$probe_pid" ]] || wait "$probe_pid" 2>/dev/null || true
    [[ -z "$xray_pid" ]] || wait "$xray_pid" 2>/dev/null || true
    rm -rf -- "$test_dir"
}
trap cleanup EXIT

wsl_address="$(hostname -I | awk '{print $1}')"
if [[ -z "$wsl_address" ]]; then
    echo "Could not determine the WSL address" >&2
    exit 1
fi

authority_json=""
if [[ -n "$authority" ]]; then
    authority_json=", \"authority\": \"$authority\""
fi
cat >"$test_dir/xray.json" <<JSON
{
  "log": {"loglevel": "warning"},
  "inbounds": [{
    "listen": "0.0.0.0", "port": 2102, "protocol": "socks",
    "settings": {"udp": true}
  }],
  "outbounds": [{
    "protocol": "vless",
    "settings": {"vnext": [{
      "address": "$host", "port": $port,
      "users": [{"id": "$uuid", "encryption": "none"}]
    }]},
    "streamSettings": {
      "network": "grpc", "security": "reality",
      "realitySettings": {
        "serverName": "$sni", "fingerprint": "chrome",
        "publicKey": "$public_key", "shortId": "$short_id"
      },
      "grpcSettings": {
        "serviceName": "$service_name", "multiMode": false$authority_json
      }
    }
  }]
}
JSON

"$xray_bin" run -c "$test_dir/xray.json" >"$test_dir/xray.log" 2>&1 &
xray_pid=$!
sleep 1
official_code="$(curl -sS --max-time 20 --proxy socks5h://127.0.0.1:2102 \
    -o "$test_dir/official.json" -w '%{http_code}' \
    https://discord.com/api/v10/gateway)"
if [[ "$official_code" != "200" ]] ||
   ! grep -q '"url":"wss://gateway.discord.gg"' "$test_dir/official.json"; then
    echo "Official Xray profile control failed: HTTP $official_code" >&2
    tail -80 "$test_dir/xray.log" >&2
    exit 1
fi

if ! "$probe_bin" --socks-udp "$wsl_address" 2102 \
    >"$test_dir/official-udp.log" 2>&1; then
    echo "Official Xray real-profile UDP control failed" >&2
    tail -100 "$test_dir/official-udp.log" >&2
    tail -100 "$test_dir/xray.log" >&2
    exit 1
fi

"$probe_bin" --grpc-socks-uri "$profile_uri" 2100 8 \
    >"$test_dir/probe.log" 2>&1 &
probe_pid=$!
for attempt in {1..15}; do
    grep -q grpc_socks_ready "$test_dir/probe.log" && break
    kill -0 "$probe_pid" 2>/dev/null || break
    sleep 1
done
if ! grep -q grpc_socks_ready "$test_dir/probe.log"; then
    echo "BigHeadVPN gRPC probe did not start SOCKS5" >&2
    tail -100 "$test_dir/probe.log" >&2
    exit 1
fi

native_code="$(/mnt/c/Windows/System32/curl.exe -sS --max-time 20 \
    --proxy socks5h://127.0.0.1:2100 -o NUL -w '%{http_code}' \
    https://discord.com/api/v10/gateway)"
if [[ "$native_code" != "$official_code" ]]; then
    echo "Real gRPC mismatch: official=$official_code native=$native_code" >&2
    tail -100 "$test_dir/probe.log" >&2
    exit 1
fi

if ! "$probe_bin" --tunnel-udp-uri "$profile_uri" \
    >"$test_dir/native-udp.log" 2>&1; then
    echo "BigHeadVPN real-profile UDP test failed" >&2
    tail -100 "$test_dir/native-udp.log" >&2
    exit 1
fi

native_udp="$(tail -1 "$test_dir/native-udp.log")"
echo "VLESS gRPC real profile passed: official=$official_code native=$native_code; $native_udp"
