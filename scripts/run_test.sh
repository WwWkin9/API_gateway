#!/bin/bash
set -e
cd /home/lin/API_gateway

# Clean up any leftover
kill $(pgrep -f "build/order_server") 2>/dev/null || true
kill $(pgrep -f "build/user_server") 2>/dev/null || true
kill $(pgrep -f "build/gateway") 2>/dev/null || true
sleep 1

# Start backends
build/order_server &
build/user_server &
sleep 0.5

# Start gateway in foreground, background, and immediately test
build/gateway --config=config.json &
GW_PID=$!
sleep 1

echo "=== curl test ==="
curl -s --max-time 3 http://127.0.0.1:8080/health
echo ""
curl -s --max-time 3 http://127.0.0.1:8080/api/user
echo ""

# Quick wrk
echo "=== wrk 50conn 5s ==="
wrk -t4 -c50 -d5s -s scripts/wrk_mixed.lua http://127.0.0.1:8080

# Cleanup
kill $GW_PID 2>/dev/null
kill %1 %2 2>/dev/null
wait 2>/dev/null
echo "=== done ==="
