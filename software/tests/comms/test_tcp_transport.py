"""End-to-end tests for the TCP (Ethernet) transport.

These run :class:`ProtocolClient` over a real loopback socket against a mock board server that
frames responses exactly like the firmware's ``NetCli.c`` (``key=value`` lines + ``crc=HH`` +
blank line, ``\\r``/``\\n`` terminated). No hardware needed — this proves the same client that
talks RS-485 talks Ethernet with only the byte pipe swapped, including the drop/reconnect path.
"""
import asyncio
import socket
import threading

from dro.comms.protocol_client import ProtocolClient, xor8
from dro.comms.tcp_transport import TcpTransport


# ── mock board TCP server (one client at a time, like the W5500 CLI) ─────────
class MockBoardServer:
    """Listens on 127.0.0.1:<ephemeral> and answers protocol lines via ``responder``.

    ``responder(request_body) -> list[str] | None`` returns body lines (no crc); the server
    appends a correct ``crc=HH`` + blank-line terminator. Returning ``None`` sends nothing
    (simulates a silent/dead board). Stays listening across connections so a client can
    reconnect after a drop.
    """

    def __init__(self, responder):
        self.responder = responder
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(1)
        self.port = self._sock.getsockname()[1]
        self.connections = 0
        self._active = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        self._sock.settimeout(0.2)
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.connections += 1
            self._active = conn
            self._handle(conn)
            self._active = None

    def _handle(self, conn):
        conn.settimeout(0.2)
        buf = b""
        while not self._stop.is_set():
            try:
                data = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            buf += data
            while True:
                idxs = [i for i in (buf.find(b"\r"), buf.find(b"\n")) if i != -1]
                if not idxs:
                    break
                cut = min(idxs)
                line, buf = buf[:cut], buf[cut + 1:]
                text = line.decode("ascii", "replace").strip()
                if not text:
                    continue
                body = text.rsplit("*", 1)[0] if "*" in text else text
                out = self.responder(body)
                if out is None:
                    continue
                frame = "".join(l + "\n" for l in out)
                frame += f"crc={xor8(frame.encode()):02X}\n\n"
                try:
                    conn.sendall(frame.encode("ascii"))
                except OSError:
                    return
        try:
            conn.close()
        except OSError:
            pass

    def drop_current(self):
        """Force-close the active client connection (simulates the board dropping the link)."""
        c = self._active
        if c is not None:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                c.close()
            except OSError:
                pass

    def close(self):
        self._stop.set()
        try:
            self._sock.close()
        except OSError:
            pass
        self._thread.join(timeout=1.0)


def board_responder():
    """A 5-scale V1.5 board stand-in with a couple of writable fields + scales.count."""
    state = {"servo.max": "720", "servo.acc": "120"}

    def responder(body):
        parts = body.split()
        if not parts:
            return ["error=empty command"]
        cmd = parts[0]
        if cmd == "version":
            return ["version=v1.5.0-test"]
        if cmd == "sta":
            return ["scales.pos=0,0,0,0,0", "scales.speed=0,0,0,0,0",
                    "servo.pos=0", "servo.speed=0", "servo.tgt=0", "servo.mode=0",
                    "din.state=0"]
        if cmd == "get":
            name = parts[1]
            if name == "scales.count":
                return ["scales.count=5"]
            return [f"{name}={state[name]}"] if name in state else ["error=unknown variable"]
        if cmd == "set":
            name, val = parts[1], parts[2]
            if name not in state:
                return ["error=unknown variable"]
            state[name] = val
            return []
        return ["error=unknown command"]

    return responder


def _run(coro):
    return asyncio.run(coro)


# ── ProtocolClient over TCP ──────────────────────────────────────────────────
def test_tcp_version_roundtrip():
    srv = MockBoardServer(board_responder())

    async def go():
        c = ProtocolClient(host="127.0.0.1", tcp_port=srv.port)
        assert c.kind == "tcp"
        assert c.description == f"tcp://127.0.0.1:{srv.port}"
        await c.open()
        try:
            assert await c.version() == "v1.5.0-test"
            assert c.connected is True
        finally:
            await c.close()

    try:
        _run(go())
    finally:
        srv.close()


def test_tcp_sta_and_set_get():
    srv = MockBoardServer(board_responder())

    async def go():
        c = ProtocolClient(host="127.0.0.1", tcp_port=srv.port)
        await c.open()
        try:
            r = await c.sta()
            assert r.crc_ok
            assert r.as_ints("scales.pos") == [0, 0, 0, 0, 0]
            assert await c.set("servo.max", 500.0)
            assert (await c.get("servo.max")).text("servo.max") == "500"
        finally:
            await c.close()

    try:
        _run(go())
    finally:
        srv.close()


def test_tcp_scales_count_reports_five():
    srv = MockBoardServer(board_responder())

    async def go():
        c = ProtocolClient(host="127.0.0.1", tcp_port=srv.port)
        await c.open()
        try:
            assert (await c.get("scales.count")).as_int("scales.count") == 5
        finally:
            await c.close()

    try:
        _run(go())
    finally:
        srv.close()


def test_tcp_blank_host_stays_disconnected():
    """TCP selected but no IP set → open() fails cleanly (no serial fallback), link stays down."""
    async def go():
        c = ProtocolClient(host="", tcp_port=5555)
        assert c.kind == "tcp"
        raised = False
        try:
            await c.open()
        except OSError:
            raised = True
        assert raised and not c.is_open

    _run(go())


def test_tcp_reconnect_after_board_drop():
    srv = MockBoardServer(board_responder())

    async def go():
        c = ProtocolClient(host="127.0.0.1", tcp_port=srv.port, max_errors=1)
        await c.open()
        try:
            assert await c.version() == "v1.5.0-test"
            assert srv.connections == 1

            srv.drop_current()                     # board drops the link
            await asyncio.sleep(0.1)
            # Next command hits the dead socket: transport is flagged broken and dropped.
            await c.command("version", timeout=0.3, retries=1)
            assert c.is_open is False

            # The poll loop's reopen: a fresh connect succeeds against the still-listening board.
            await c.open()
            assert c.is_open is True
            assert await c.version() == "v1.5.0-test"
            assert srv.connections == 2
        finally:
            await c.close()

    try:
        _run(go())
    finally:
        srv.close()


# ── TcpTransport unit behaviour ──────────────────────────────────────────────
def test_transport_read_timeout_returns_empty():
    srv = MockBoardServer(lambda body: None)       # never replies
    try:
        t = TcpTransport("127.0.0.1", srv.port, timeout=0.1)
        t.write(b"version\r")
        assert t.read(1) == b""                     # timed out, no bytes → b"" (pyserial parity)
        t.close()
    finally:
        srv.close()


def test_transport_raises_on_closed_socket():
    srv = MockBoardServer(board_responder())
    t = TcpTransport("127.0.0.1", srv.port, timeout=0.2)
    srv.drop_current()
    import time
    time.sleep(0.1)
    raised = False
    try:
        # Read enough to run past any buffered bytes into the closed state.
        for _ in range(10):
            if t.read(1) == b"":
                continue
    except ConnectionError:
        raised = True
    finally:
        t.close()
        srv.close()
    assert raised