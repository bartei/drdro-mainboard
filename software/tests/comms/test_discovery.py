"""Tests for local board discovery (probe + subnet sweep) against loopback mock boards."""
import asyncio
import ipaddress
import socket

from dro.comms import discovery
from test_tcp_transport import MockBoardServer, board_responder


def _run(coro):
    return asyncio.run(coro)


def test_probe_finds_board():
    srv = MockBoardServer(board_responder())
    try:
        r = _run(discovery.probe("127.0.0.1", srv.port, timeout=1.0))
        assert r is not None
        assert r["host"] == "127.0.0.1"
        assert r["port"] == srv.port
        assert r["version"] == "v1.5.0-test"
    finally:
        srv.close()


def test_probe_no_listener_returns_none():
    s = socket.socket()                      # bind then close → a port with nothing listening
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    assert _run(discovery.probe("127.0.0.1", port, timeout=0.3)) is None


def test_probe_silent_service_returns_none():
    srv = MockBoardServer(lambda body: None)   # accepts but never replies
    try:
        assert _run(discovery.probe("127.0.0.1", srv.port, timeout=0.3)) is None
    finally:
        srv.close()


def test_probe_valid_frame_without_version_is_not_a_board():
    srv = MockBoardServer(lambda body: ["hello=world"])   # crc-valid, but not a board
    try:
        assert _run(discovery.probe("127.0.0.1", srv.port, timeout=0.5)) is None
    finally:
        srv.close()


def test_local_scan_hosts_excludes_own_and_loopback():
    hosts = discovery.local_scan_hosts()
    assert isinstance(hosts, list)
    assert "127.0.0.1" not in hosts
    assert discovery._local_ipv4s().isdisjoint(hosts)
    for h in hosts[:3] + hosts[-3:]:
        ipaddress.IPv4Address(h)             # every entry is a valid IPv4


def test_discover_sweeps_patched_subnet(monkeypatch):
    srv = MockBoardServer(board_responder())
    seen = {"progress": 0}

    def on_progress(done, total, result):
        seen["progress"] = done

    try:
        # Point the sweep at loopback: .1 answers, .2 does not.
        monkeypatch.setattr(discovery, "local_scan_hosts", lambda: ["127.0.0.1", "127.0.0.2"])
        results = _run(discovery.discover(port=srv.port, timeout=0.5, on_progress=on_progress))
        assert any(r["host"] == "127.0.0.1" and r["port"] == srv.port for r in results)
        assert seen["progress"] == 2         # progress fired for every host
    finally:
        srv.close()