#!/usr/bin/env python3
"""drDRO firmware updater — dual-bank update over RS485/UART or Ethernet.

Serial path (via the IAP bootloader, design: dualbank_design.md):
  1. (app)  `update`            -> reboot into the bootloader CLI ("bootloader=ready")
  2. (boot) `info`              -> pick the inactive bank (unless --bank given)
  3. (boot) `flash <bank>`      -> YMODEM-send the .bin into that bank
  4. (boot) `bank <bank>`       -> select it as the active bank (persisted)
  5. (boot) `boot`              -> copy active bank -> Exec, jump to the new app

Network path (--net; the RUNNING APP writes the bank, bootloader stays UART-only):
  1. `bank`                      -> pick the inactive bank (unless --bank given)
  2. `fw.begin <bank> <size> <crc32>` -> board listens on TCP cli-port+1
  3. stream the raw .bin to that data port, close
  4. `fw.status` until done      -> stream CRC verified on the board
  5. `fw.commit`                 -> bank CRC recorded + bank selected (one save)
  6. `reset`                     -> bootloader copies bank -> Exec, runs it

Self-contained YMODEM sender (CRC-16, 1024-byte STX blocks) — no lrzsz needed; matches
bootloader/src/ymodem.c. Serial baud is fixed at 115200 (hardware limit).

Usage:
  ./dro_update.py /dev/ttyACM0 firmware.bin              # serial, auto-pick bank
  ./dro_update.py /dev/ttyACM0 firmware.bin --bank 1     # serial, force a bank
  ./dro_update.py /dev/ttyACM0 firmware.bin --in-bootloader --no-boot
  ./dro_update.py --net 10.1.2.105 firmware.bin          # Ethernet (CLI port 5555)
  ./dro_update.py --net 10.1.2.105:5555 firmware.bin --bank 1
"""
import argparse
import os
import socket as pysocket
import sys
import time
import zlib

def _need_pyserial():
    try:
        import serial  # pyserial
        return serial
    except ImportError:
        sys.exit("pyserial not found — install it (e.g. `pip install pyserial`).")

SOH, STX, EOT, ACK, NAK, CAN, CRC_C, SUB = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43, 0x1A
DATA_LEN = 1024


# ---- YMODEM (sender) -------------------------------------------------------
def crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def make_block(seq: int, payload: bytes) -> bytes:
    head = SOH if len(payload) == 128 else STX
    c = crc16(payload)
    return bytes([head, seq & 0xFF, (~seq) & 0xFF]) + payload + bytes([c >> 8, c & 0xFF])


def wait_for(ser, want, timeout):
    """Discard bytes until `want` arrives (tolerates the RS485 turnaround glitch)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        b = ser.read(1)
        if b and b[0] == want:
            return True
    return False


def wait_ack(ser, timeout=2.0):
    """Wait for ACK/NAK/CAN. A real YMODEM abort is CAN CAN — a single 0x18 can
    be line noise (RS-485 bus contention garbage), so require two in a row."""
    deadline = time.monotonic() + timeout
    last_can = False
    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == CAN:
            if last_can:
                return CAN
            last_can = True
            continue
        last_can = False
        if b[0] in (ACK, NAK):
            return b[0]
    return None


def send_block(ser, block, retries=10, ack_timeout=2.0):
    for _ in range(retries):
        ser.write(block); ser.flush()
        r = wait_ack(ser, ack_timeout)
        if r == ACK:
            return True
        if r == CAN:
            sys.exit("bootloader cancelled the transfer (CAN CAN).")
    return False


def ymodem_send(ser, path):
    data = open(path, "rb").read()
    name = os.path.basename(path).encode()
    size = len(data)
    print(f"  YMODEM: {name.decode()} ({size} bytes)")
    if not wait_for(ser, CRC_C, 30.0):
        sys.exit("no YMODEM handshake ('C') — did `flash <bank>` start?")
    header = (name + b"\x00" + str(size).encode() + b"\x00").ljust(128, b"\x00")
    if not send_block(ser, make_block(0, header)):
        sys.exit("header (block 0) not acked.")
    if not wait_for(ser, CRC_C, 5.0):
        sys.exit("no 'C' after header.")
    seq = 1
    for off in range(0, size, DATA_LEN):
        chunk = data[off:off + DATA_LEN].ljust(DATA_LEN, bytes([SUB]))
        # The FIRST data block triggers the receiver's lazy 128K sector erase
        # (up to ~4 s of CPU stall): retransmitting into that window collides
        # with the (delayed) ACK on the half-duplex bus. Wait it out instead.
        if not send_block(ser, make_block(seq, chunk),
                          ack_timeout=(8.0 if seq == 1 else 2.0)):
            sys.exit(f"block {seq} not acked.")
        seq += 1
        print(f"\r  {min(off + DATA_LEN, size)}/{size} bytes", end="", flush=True)
    print()
    for _ in range(10):                       # EOT (NAK'd once, then ACK'd)
        ser.write(bytes([EOT])); ser.flush()
        if wait_ack(ser) == ACK:
            break
    else:
        sys.exit("EOT not acked.")
    if wait_for(ser, CRC_C, 5.0):             # trailing null header closes the batch
        send_block(ser, make_block(0, b"\x00" * 128))


# ---- CLI (same wire format as the app) -------------------------------------
def read_response(ser, timeout=3.0):
    """Read a framed response (key=value lines until a blank line). Returns a dict."""
    deadline = time.monotonic() + timeout
    buf = b""
    kv = {}
    seen = False
    while time.monotonic() < deadline:
        c = ser.read(1)
        if not c:
            continue
        if c == b"\n":
            line = buf.decode("ascii", "replace").replace("\r", "").strip()
            buf = b""
            if line == "":
                if seen:
                    return kv
                continue
            seen = True
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip()
        else:
            buf += c
    return kv


def cli(ser, cmd, timeout=3.0, retries=3):
    """Send a CLI command and return the parsed framed response. Retries on an empty or
    'unknown command' reply: the first byte of a command can be lost to the RS485
    turnaround right after the device transmitted (e.g. just after the greeting), and
    these commands are all valid — so a glitch, not a real error."""
    resp = {}
    for _ in range(retries):
        ser.reset_input_buffer()
        ser.write((cmd + "\r").encode()); ser.flush()
        resp = read_response(ser, timeout)
        if resp and resp.get("error") != "unknown command" and "error" not in resp:
            return resp
        time.sleep(0.15)
    if "error" in resp:
        sys.exit(f"`{cmd}` -> error={resp['error']}")
    return resp


def enter_bootloader(ser):
    print("requesting update (-> bootloader)...")
    ser.reset_input_buffer()
    ser.write(b"update\r"); ser.flush()
    # The app acks `update=ready` then jumps; the bootloader greets `bootloader=ready`.
    # Wait until we've seen "bootloader" AND the frame's terminating blank line (\n\n):
    # the substring match tolerates a glitched first greeting byte (RS485 turnaround),
    # and waiting for the terminator guarantees the bootloader finished transmitting (is
    # back in RX) before we send a command — so our command's first byte isn't lost.
    deadline = time.monotonic() + 8.0
    buf = b""
    while time.monotonic() < deadline:
        c = ser.read(1)
        if not c:
            continue
        buf += c
        if b"bootloader" in buf and buf.endswith(b"\n\n"):
            time.sleep(0.15)            # let the bootloader's TX->RX turnaround settle
            ser.reset_input_buffer()
            return
    sys.exit("bootloader did not announce itself (no 'bootloader=ready').")


# ---- network path (TCP CLI + raw data socket; app-side bank writer) --------
class NetCli:
    def __init__(self, host, port, timeout=8.0):
        self.sock = pysocket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def cmd(self, c, timeout=8.0):
        """Send one command, return the framed response as a dict."""
        self.sock.settimeout(timeout)
        self.sock.sendall(c.encode() + b"\r")
        buf = b""
        while not buf.endswith(b"\n\n"):
            d = self.sock.recv(512)
            if not d:
                sys.exit(f"connection closed during `{c}`")
            buf += d
        kv = {}
        for line in buf.decode("ascii", "replace").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip()
        return kv

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def net_update(host, cliport, args):
    data = open(args.binary, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF        # same CRC-32 the board computes

    cli = NetCli(host, cliport)
    ver = cli.cmd("version").get("version", "?")
    print(f"connected to {host}:{cliport}, running version={ver}")

    bank = args.bank
    if bank is None:
        active = int(cli.cmd("bank").get("bank.active", "0"))
        bank = 1 - active
        print(f"active bank is {active} -> flashing inactive bank {bank}")

    # fw.begin answers after arming the listener; the erase happens lazily on
    # the first data chunk, so give the status polls generous timeouts instead.
    r = cli.cmd(f"fw.begin {bank} {len(data)} {crc:08X}", timeout=10.0)
    if "error" in r:
        sys.exit(f"fw.begin -> error={r['error']}")
    dport = int(r.get("fw.port", cliport + 1))
    print(f"streaming {len(data)} bytes to {host}:{dport} ...")

    ds = pysocket.create_connection((host, dport), timeout=10.0)
    ds.sendall(data)
    ds.close()

    deadline = time.monotonic() + 120.0
    while time.monotonic() < deadline:
        st = cli.cmd("fw.status", timeout=10.0)
        state = st.get("fw.state", "?")
        print(f"\r  fw.state={state} fw.recv={st.get('fw.recv', '?')}   ", end="", flush=True)
        if state == "done":
            print()
            break
        if state == "error":
            print()
            sys.exit(f"transfer failed: fw.reason={st.get('fw.reason', '?')}")
        time.sleep(0.5)
    else:
        sys.exit("timed out waiting for the transfer to complete")

    r = cli.cmd("fw.commit", timeout=15.0)      # includes a 128K CRC + settings save
    if "error" in r:
        sys.exit(f"fw.commit -> error={r['error']}")
    print(f"committed: bank={r.get('fw.bank','?')} region crc={r.get('fw.crc','?')}")

    if args.no_boot:
        print("done (--no-boot): send `reset` to boot the new image.")
        cli.close()
        return

    cli.cmd("reset")
    cli.close()
    print("resetting (bootloader copies bank -> Exec) ...")
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        time.sleep(2.0)
        try:
            cli = NetCli(host, cliport, timeout=4.0)
            ver = cli.cmd("version").get("version", "?")
            print(f"app is up: version={ver}")
            cli.close()
            return
        except OSError:
            continue
    sys.exit("board did not come back on the network — check the LED / RS-485.")


def main():
    ap = argparse.ArgumentParser(description="drDRO dual-bank firmware updater (YMODEM over UART/RS485, or TCP with --net).")
    ap.add_argument("port", help="serial device, or <ip>[:port] with --net")
    ap.add_argument("binary", help="app firmware .bin (linked for the Exec region)")
    ap.add_argument("--net", action="store_true", help="update over Ethernet via the app's fw.* commands")
    ap.add_argument("--bank", type=int, choices=(0, 1), help="target bank (default: the inactive one)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--in-bootloader", action="store_true", help="board is already in the bootloader CLI (serial only)")
    ap.add_argument("--no-boot", action="store_true", help="flash + select the bank but don't boot")
    args = ap.parse_args()
    if not os.path.isfile(args.binary):
        sys.exit(f"no such file: {args.binary}")

    if args.net:
        host, _, p = args.port.partition(":")
        net_update(host, int(p) if p else 5555, args)
        return

    serial = _need_pyserial()
    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        if not args.in_bootloader:
            enter_bootloader(ser)

        bank = args.bank
        if bank is None:
            info = cli(ser, "info")
            active = int(info.get("bank.active", "0"))
            bank = 1 - active
            print(f"active bank is {active} -> flashing inactive bank {bank}")

        print(f"flashing bank {bank} ...")
        ser.reset_input_buffer()
        ser.write(f"flash {bank}\r".encode()); ser.flush()
        ymodem_send(ser, args.binary)
        res = read_response(ser, 5.0)
        if "error" in res or "flash" not in res:
            sys.exit(f"flash failed: {res}")
        print(f"  bank {bank} written ({res.get('size','?')} bytes)")

        cli(ser, f"bank {bank}")
        print(f"selected bank {bank} as active")

        if args.no_boot:
            print("done (--no-boot): send `boot` to run it.")
            return
        ser.write(b"boot\r"); ser.flush()      # copies active bank -> Exec, jumps (no framed reply)
        print("booting new image (copying bank -> Exec) ...")
        time.sleep(2.0)
        # The very first command after the jump can lose its leading byte to the RS485
        # turnaround as the app's USART comes up, so retry the version read a few times.
        for _ in range(6):
            ser.reset_input_buffer()
            ser.write(b"version\r"); ser.flush()
            resp = read_response(ser, 1.5)
            if "version" in resp:
                print(f"app is up: version={resp['version']}")
                return
            time.sleep(0.4)
        print("booted, but no version response yet — check manually with `version`.")


if __name__ == "__main__":
    main()
