#!/bin/bash
# Install the Clawdmeter daemon as a per-user launchd agent on macOS.
#
# Creates an isolated Python venv inside daemon-macos/.venv so the system
# Python stays untouched, installs `bleak`, generates a launchd plist with
# absolute paths, and loads it. Idempotent — safe to run repeatedly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DAEMON_DIR="$SCRIPT_DIR/daemon-macos"
VENV_DIR="$DAEMON_DIR/.venv"
SCRIPT="$DAEMON_DIR/clawdmeter_daemon.py"
TEMPLATE="$DAEMON_DIR/com.clawdmeter.daemon.plist.in"
LABEL="com.clawdmeter.daemon"
PLIST_DST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs/Clawdmeter"

echo "=== Clawdmeter daemon — macOS install ==="

# 1) Dependencies
echo "[1/5] Checking dependencies..."
command -v python3 >/dev/null || { echo "  python3 not found — install Xcode CLT or Homebrew Python"; exit 1; }
command -v security >/dev/null || { echo "  security CLI missing (Apple)"; exit 1; }
echo "  ok"

# 2) Venv + bleak
echo "[2/5] Setting up Python venv in $VENV_DIR..."
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip >/dev/null
"$VENV_DIR/bin/pip" install --quiet -r "$DAEMON_DIR/requirements.txt"
echo "  ok"

# 3) Log dir
echo "[3/5] Ensuring log directory $LOG_DIR..."
mkdir -p "$LOG_DIR"
echo "  ok"

# 4) Render plist with absolute paths
echo "[4/5] Writing $PLIST_DST..."
mkdir -p "$(dirname "$PLIST_DST")"
sed \
    -e "s|__PYTHON__|$VENV_DIR/bin/python3|g" \
    -e "s|__SCRIPT__|$SCRIPT|g" \
    -e "s|__LOGDIR__|$LOG_DIR|g" \
    "$TEMPLATE" > "$PLIST_DST"
echo "  ok"

# 5) Load (or reload) the agent. `bootout` may fail on first run — that's fine.
echo "[5/5] Loading launchd agent..."
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST_DST"
launchctl enable "gui/$(id -u)/$LABEL" 2>/dev/null || true
echo "  ok"

cat <<EOF

=== Done ===

The daemon is running and will auto-start at every login.

Logs:
  tail -f $LOG_DIR/clawdmeter.out.log
  tail -f $LOG_DIR/clawdmeter.err.log

Manage:
  launchctl kickstart -k gui/$(id -u)/$LABEL   # restart
  launchctl bootout   gui/$(id -u)/$LABEL      # stop and unload

First run will:
  1) Read your Claude OAuth token from the macOS keychain
     (entry "Claude Code-credentials")
  2) Scan for "Claude Controller" over BLE — make sure the
     device is powered and showing the Bluetooth screen
  3) Cache the device address at ~/.config/claude-usage-monitor/ble-address
  4) Poll the Anthropic API every 60s and push usage to the device

macOS will pop a Bluetooth permission prompt the first time. Approve it
under System Settings → Privacy & Security → Bluetooth for the python3
inside daemon-macos/.venv/bin.
EOF
