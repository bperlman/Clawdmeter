#!/usr/bin/env python3
"""
Clawdmeter daemon — macOS edition.

Reads the Claude Code OAuth token from the macOS Keychain, polls the
Anthropic API every POLL_INTERVAL seconds, and writes a JSON usage
payload to the Clawd Controller ESP32 over BLE GATT.

Mirrors the Linux bash daemon (daemon/claude-usage-daemon.sh) but
talks CoreBluetooth via the `bleak` library instead of bluez/dbus.

Same characteristic UUIDs as the Linux daemon — no firmware changes.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.backends.corebluetooth.CentralManagerDelegate import CentralManagerDelegate
from bleak.backends.device import BLEDevice
from CoreBluetooth import CBUUID

# ---- Configuration ----
DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"  # daemon writes here
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"  # firmware notifies here
POLL_INTERVAL = 15   # seconds between Anthropic polls
TICK = 5             # inner-loop sleep
SCAN_TIMEOUT = 10
CONNECT_TIMEOUT = 10

# Keychain entry written by Claude Code on macOS.
KEYCHAIN_SERVICE = "Claude Code-credentials"

CACHE_DIR = Path.home() / ".config" / "claude-usage-monitor"
CACHE_FILE = CACHE_DIR / "ble-address"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("clawdmeter")


# ============================================================================
# OAuth token (macOS keychain)
# ============================================================================

def read_token() -> str:
    """Read the Claude Code OAuth access token from the macOS keychain.

    `security` writes the password to stderr by default (the `-w` flag moves
    it to stdout — both go to *this* process, never to the terminal or
    transcript). The keychain value is a JSON blob with shape:
        {"claudeAiOauth": {"accessToken": "...", ...}}
    """
    try:
        result = subprocess.run(
            ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
            capture_output=True,
            text=True,
            check=True,
            timeout=10,
        )
    except subprocess.CalledProcessError:
        raise RuntimeError(
            f"Could not read '{KEYCHAIN_SERVICE}' from keychain. "
            "Sign in to Claude Code first."
        )
    raw = result.stdout.strip()
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        # Some Claude Code versions store the bare token string.
        return raw
    # New format wraps it inside claudeAiOauth.accessToken.
    if isinstance(parsed, dict):
        for path in (("claudeAiOauth", "accessToken"), ("accessToken",)):
            cur = parsed
            for key in path:
                if not isinstance(cur, dict) or key not in cur:
                    cur = None
                    break
                cur = cur[key]
            if isinstance(cur, str) and cur:
                return cur
    raise RuntimeError("Keychain value has unexpected shape (no accessToken)")


# ============================================================================
# Anthropic poll
# ============================================================================

def poll_anthropic(token: str) -> dict:
    """Hit the messages API with a 1-token request, parse rate-limit headers
    from the response, return the payload dict the firmware expects.
    """
    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        method="POST",
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "Content-Type": "application/json",
            "User-Agent": "claude-code/2.1.5",
        },
        data=json.dumps({
            "model": "claude-haiku-4-5-20251001",
            "max_tokens": 1,
            "messages": [{"role": "user", "content": "hi"}],
        }).encode("utf-8"),
    )

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            headers = {k.lower(): v for k, v in resp.headers.items()}
    except urllib.error.HTTPError as e:
        # Even 4xx responses carry the rate-limit headers we need.
        headers = {k.lower(): v for k, v in e.headers.items()}
    except urllib.error.URLError as e:
        raise RuntimeError(f"API call failed: {e}")

    def f(key: str, default: float = 0.0) -> float:
        try:
            return float(headers.get(key, default))
        except (TypeError, ValueError):
            return default

    now = int(time.time())
    s5_util = f("anthropic-ratelimit-unified-5h-utilization")
    s5_reset = f("anthropic-ratelimit-unified-5h-reset")
    s7_util = f("anthropic-ratelimit-unified-7d-utilization")
    s7_reset = f("anthropic-ratelimit-unified-7d-reset")
    status = headers.get("anthropic-ratelimit-unified-5h-status", "unknown")

    return {
        "s": round(s5_util * 100),
        "sr": max(0, round((s5_reset - now) / 60)) if s5_reset else 0,
        "w": round(s7_util * 100),
        "wr": max(0, round((s7_reset - now) / 60)) if s7_reset else 0,
        "st": status,
        "ok": True,
    }


# ============================================================================
# BLE device discovery + address cache
# ============================================================================

def load_cached_address() -> str | None:
    if not CACHE_FILE.exists():
        return None
    try:
        addr = CACHE_FILE.read_text().strip()
        return addr or None
    except OSError:
        return None


def save_cached_address(addr: str) -> None:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    CACHE_FILE.write_text(addr + "\n")


def drop_cached_address() -> None:
    try:
        CACHE_FILE.unlink()
    except FileNotFoundError:
        pass


async def discover_device() -> str | None:
    """Scan for DEVICE_NAME and return its bleak address (a CoreBluetooth
    UUID on macOS, a MAC on Linux). bleak's UUIDs are stable per host/device
    pair, so caching is fine.
    """
    log.info("Scanning for '%s' (%ds)...", DEVICE_NAME, SCAN_TIMEOUT)
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log.info("Found %s at %s", d.name, d.address)
            return d.address
    return None


# The device advertises only while it has no connection. macOS bonds with it
# as a BLE HID keyboard, so after every device reboot the OS usually wins the
# reconnect race — advertising stops and a scan will never see it. CoreBluetooth
# can still hand us the already-connected peripheral; bleak just doesn't expose
# that, so we call retrieveConnectedPeripheralsWithServices: ourselves and wrap
# the result in a BLEDevice (details must be the (CBPeripheral, manager) pair
# BleakClient expects). The manager must outlive the connection — the caller
# holds it via the returned BLEDevice's details.
async def find_system_connected_device() -> BLEDevice | None:
    mgr = CentralManagerDelegate()
    await mgr.wait_until_ready()
    peripherals = mgr.central_manager.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in peripherals:
        if p.name() == DEVICE_NAME:
            addr = str(p.identifier().UUIDString())
            log.info("Found %s already system-connected at %s", DEVICE_NAME, addr)
            return BLEDevice(addr, str(p.name()), (p, mgr))
    return None


# ============================================================================
# Connection loop
# ============================================================================

class Daemon:
    def __init__(self) -> None:
        self.refresh_requested = asyncio.Event()
        self.stop = asyncio.Event()

    def _on_refresh(self, _handle: int, data: bytearray) -> None:
        """REQ characteristic notify handler — the firmware sets this to
        ask the daemon to poll Anthropic right now (e.g. fresh boot).
        """
        log.info("Refresh requested by device (data=%s)", bytes(data).hex())
        self.refresh_requested.set()

    async def connect(self, target: str | BLEDevice) -> BleakClient | None:
        address = target.address if isinstance(target, BLEDevice) else target
        log.info("Connecting to %s...", address)
        client = BleakClient(target, timeout=CONNECT_TIMEOUT)
        try:
            await client.connect()
        except Exception as e:
            log.warning("Connect failed: %s", e)
            return None
        if not client.is_connected:
            log.warning("Connect returned but is_connected=False")
            return None
        log.info("Connected")
        # Subscribe to the refresh-request characteristic. If it's missing
        # we still succeed — the daemon's poll-on-interval loop is enough.
        try:
            await client.start_notify(REQ_CHAR_UUID, self._on_refresh)
            log.info("Subscribed to refresh-request channel")
        except Exception as e:
            log.warning("Could not subscribe to REQ char: %s", e)
        return client

    async def send_payload(self, client: BleakClient, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        try:
            await client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            log.info("Sent: %s", data.decode())
            return True
        except Exception as e:
            log.warning("Write failed: %s", e)
            return False

    async def run(self) -> None:
        backoff = 1
        while not self.stop.is_set():
            # If macOS already holds the connection (bonded HID keyboard),
            # the device isn't advertising and neither the cached address
            # nor a scan can reach it — but this can.
            target: str | BLEDevice | None = await find_system_connected_device()
            if target is None:
                target = load_cached_address()
            if target is None:
                target = await discover_device()
                if target is None:
                    log.info("Device not found, retrying in %ds...", backoff)
                    await self._sleep_or_stop(backoff)
                    backoff = min(backoff * 2, 60)
                    continue
                save_cached_address(target)

            client = await self.connect(target)
            if client is None:
                log.info("Dropping cached address and retrying in %ds...", backoff)
                drop_cached_address()
                await self._sleep_or_stop(backoff)
                backoff = min(backoff * 2, 60)
                continue

            backoff = 1
            try:
                await self._connected_loop(client)
            finally:
                try:
                    await client.disconnect()
                except Exception:
                    pass

            if not self.stop.is_set():
                log.info("Disconnected — reconnecting in 2s...")
                await self._sleep_or_stop(2)

    async def _connected_loop(self, client: BleakClient) -> None:
        last_poll = 0.0
        while client.is_connected and not self.stop.is_set():
            now = time.time()
            need_poll = (
                self.refresh_requested.is_set()
                or (now - last_poll) >= POLL_INTERVAL
            )
            if need_poll:
                self.refresh_requested.clear()
                try:
                    token = read_token()
                    payload = poll_anthropic(token)
                except Exception as e:
                    log.warning("Poll error: %s", e)
                else:
                    if await self.send_payload(client, payload):
                        last_poll = now
            await self._sleep_or_stop(TICK)

    async def _sleep_or_stop(self, seconds: float) -> None:
        try:
            await asyncio.wait_for(self.stop.wait(), timeout=seconds)
        except asyncio.TimeoutError:
            pass


# ============================================================================
# Entry point
# ============================================================================

async def main() -> None:
    log.info("=== Clawdmeter daemon (macOS) ===")
    log.info("Poll interval: %ds", POLL_INTERVAL)

    daemon = Daemon()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, daemon.stop.set)

    try:
        await daemon.run()
    finally:
        log.info("Daemon stopped")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
