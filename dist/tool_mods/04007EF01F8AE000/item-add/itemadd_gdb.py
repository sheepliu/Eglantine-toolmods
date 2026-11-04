#!/usr/bin/env python3
"""One-shot guest-thread ItemAdd backend for Fortune's Weave build DB93B012.

The GDB stub pauses at the game's own inventory commit, resolves an IID with
the game's item lookup, and commits the selected descriptor and quantity into
the active inventory container. This file is the complete game-specific
runtime backend for the self-contained ItemAdd package.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import socket
import struct
from datetime import datetime
from pathlib import Path


PACKAGE_ROOT = Path(__file__).resolve().parent
MAIN = PACKAGE_ROOT / "main"
EDEN_LOG = Path.home() / "AppData" / "Roaming" / "eden" / "log" / "eden_log.txt"
MAIN_SHA256 = "616e7d906936ea4c2d908d0553cf69779e60242d769c40722267ee8bcb98e79a"
BUILD_ID = "db93b0125f4850601d61c3edde012f8eb2ecfe77000000000000000000000000"
CALLBACK = 0x8B97E0
LOOKUP = 0x6EA2C0
APPEND = 0x8B75A0
COMMIT = 0x8A25C0
TRAP = 0x1208E4
SIGNATURES = {
    CALLBACK: bytes.fromhex("7f2303d5ff8303d1fd7b08a9fc6f09a9"),
    LOOKUP: bytes.fromhex("7f2303d5fd7bbea9f30b00f9fd030091"),
    APPEND: bytes.fromhex("7f2303d5fd7bbba9f90b00f9f85f02a9"),
    COMMIT: bytes.fromhex("7f2303d5ff4303d1fd7b07a9fc6f08a9"),
    TRAP: bytes(8),
}
SIGNATURE_ALTERNATES = {
    COMMIT: (
        SIGNATURES[COMMIT],
        bytes.fromhex("020dff17ff4303d1fd7b07a9fc6f08a9"),  # ItemGain2x entry branch
    ),
}
IID = "IID_傷薬"  # Default used by the original fixed-IID smoke test.
MAX_PACKET = 0x4000
CONTEXT_PATH = PACKAGE_ROOT / "inventory_context.json"


def item_hash(iid: str) -> int:
    # FUN_71007AC700: FNV-1 (multiply, then XOR), over the UTF-8 bytes.
    value = 0xCBF29CE484222325
    for byte in iid.encode("utf-8"):
        value = ((value * 0x100000001B3) ^ byte) & 0xFFFFFFFFFFFFFFFF
    return value or 0xFFFFFFFFFFFFFFFF


class Remote:
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(10)

    def close(self) -> None:
        self.sock.close()

    def _byte(self) -> bytes:
        value = self.sock.recv(1)
        if not value:
            raise ConnectionError("GDB stub closed the connection")
        return value

    def reply(self) -> str:
        while self._byte() != b"$":
            pass  # Ignore acknowledgements before an asynchronous stop packet.
        buffer = bytearray()
        while (value := self._byte()) != b"#":
            buffer += value
            if len(buffer) > MAX_PACKET:
                raise ValueError("oversized GDB packet")
        checksum = int(self._byte() + self._byte(), 16)
        if sum(buffer) & 0xFF != checksum:
            self.sock.sendall(b"-")
            raise ValueError("invalid GDB packet checksum")
        self.sock.sendall(b"+")
        return buffer.decode("ascii")

    def command(self, value: str) -> str:
        payload = value.encode("ascii")
        self.sock.sendall(b"$" + payload + b"#" + f"{sum(payload) & 0xff:02x}".encode())
        result = self.reply()
        if result == "E01":
            raise RuntimeError(f"GDB stub rejected {value[:40]}")
        return result

    def resume(self) -> None:
        self.sock.sendall(b"$c#63")
        if self._byte() != b"+":
            raise RuntimeError("GDB stub did not acknowledge resume")

    def interrupt(self) -> str:
        self.sock.sendall(b"\x03")
        result = self.reply()
        if not result.startswith("T"):
            raise RuntimeError(f"GDB interrupt did not stop a thread: {result}")
        return result

    def continue_to_stop(self, timeout: int) -> str:
        self.sock.settimeout(timeout)
        try:
            result = self.command("c")
        except socket.timeout:
            self.sock.settimeout(10)
            self.interrupt()
            raise TimeoutError("no item reward before deadline; guest paused for cleanup")
        finally:
            self.sock.settimeout(10)
        if not result.startswith("T05"):
            raise RuntimeError(f"unexpected guest stop: {result}")
        return result

    def memory(self, address: int, size: int) -> bytes:
        result = self.command(f"m{address:x},{size:x}")
        value = bytes.fromhex(result)
        if len(value) != size:
            raise RuntimeError("short guest-memory read")
        return value

    def write_memory(self, address: int, value: bytes) -> None:
        if self.command(f"M{address:x},{len(value):x}:{value.hex()}") != "OK":
            raise RuntimeError("guest-memory write failed")

    def reg(self, index: int, value: int) -> None:
        if self.command(f"P{index:x}={struct.pack('<Q', value).hex()}") != "OK":
            raise RuntimeError(f"register {index} write failed")


def registers(packet: str) -> dict[str, int]:
    raw = bytes.fromhex(packet)
    # Build-194's GDBStubA64 exposes x0..x28, then fp/lr/sp/pc as
    # registers 29..32.  x30 is not emitted as the 31st 64-bit value.
    if len(raw) < 29 * 8 + 8 * 4 + 4:
        raise ValueError("incomplete AArch64 register packet")
    values = struct.unpack_from("<29Q", raw)
    lr, sp, pc = struct.unpack_from("<QQQ", raw, 29 * 8 + 8)
    return {"x0": values[0], "x1": values[1], "x2": values[2], "x3": values[3],
            "x30": lr, "sp": sp, "pc": pc}


def restore_at(packet: str, callback: int) -> str:
    """Always re-execute the original PACIBSP after removing its breakpoint."""
    byte_offset = 29 * 8 + 24
    raw = bytearray.fromhex(packet)
    struct.pack_into("<Q", raw, byte_offset, callback)
    return raw.hex()


def check_main(path: Path = MAIN) -> None:
    data = path.read_bytes()
    if (data[:4] != b"NSO0" or hashlib.sha256(data).hexdigest() != MAIN_SHA256
            or data[0x40:0x60].hex() != BUILD_ID):
        raise RuntimeError("local main is not the analyzed Fortune's Weave build")


def runtime_main_base(log_path: Path = EDEN_LOG) -> int:
    """Read the guest loader's actual main base.

    Build-194's qOffsets fallback reports the code-region start (0x08000000),
    not the relocated main module. The loader log records the module address.
    """
    if not log_path.exists():
        raise RuntimeError(f"Eglantine log not found: {log_path}")
    matches = re.findall(r"Module main loaded at (0x[0-9a-fA-F]+)",
                         log_path.read_text(encoding="utf-8", errors="replace"))
    if not matches:
        raise RuntimeError("main module load address is missing from Eglantine log")
    return int(matches[-1], 16)


def stop_thread_id(packet: str) -> int | None:
    match = re.search(r"thread:([0-9a-fA-F]+);", packet)
    return int(match.group(1), 16) if match else None


def save_inventory_context(base: int, container: int, thread_id: int | None) -> None:
    CONTEXT_PATH.parent.mkdir(parents=True, exist_ok=True)
    context = {
        "main_sha256": MAIN_SHA256,
        "main_base": hex(base),
        "container": hex(container),
        "used": False,
        "captured_at": datetime.now().isoformat(timespec="seconds"),
    }
    if thread_id is not None:
        context["thread"] = hex(thread_id)
    CONTEXT_PATH.write_text(json.dumps(context, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")


def mark_inventory_context_used() -> None:
    context = json.loads(CONTEXT_PATH.read_text(encoding="utf-8"))
    context["used"] = True
    context["used_at"] = datetime.now().isoformat(timespec="seconds")
    CONTEXT_PATH.write_text(json.dumps(context, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")


def guest_call(remote: Remote, base: int, entry: int, trap: int,
               x0: int, x1: int, stack: int, x2: int = 0, x3: int = 0) -> None:
    remote.reg(0, x0)
    remote.reg(1, x1)
    remote.reg(2, x2)
    remote.reg(3, x3)
    remote.reg(30, trap)  # LR
    remote.reg(31, stack)  # SP
    remote.reg(32, base + entry)  # PC
    remote.continue_to_stop(15)
    pc = registers(remote.command("g"))["pc"]
    if pc not in (trap, trap + 4):
        raise RuntimeError(f"helper returned to unexpected PC 0x{pc:x}")


def run(mode: str, port: int, wait: int, iid: str = IID, quantity: int = 1,
        main_path: Path = MAIN, log_path: Path = EDEN_LOG,
        skip_local_main_check: bool = False) -> dict:
    if not iid.startswith("IID_") or len(iid) > 128:
        raise ValueError("iid must be an IID_ name of at most 128 characters")
    if not 1 <= quantity <= 999:
        raise ValueError("quantity must be between 1 and 999")
    if mode == "grant" and quantity != 1:
        raise ValueError("grant mode currently supports quantity=1 only")
    if not skip_local_main_check:
        check_main(main_path)
    remote = Remote("127.0.0.1", port)
    patched: list[int] = []
    original: str | None = None
    paused = False
    calls_started = False
    completed = False
    report: dict = {"iid": iid, "quantity": quantity, "mode": mode,
                    "main_sha256": MAIN_SHA256}
    try:
        initial_stop = remote.command("?")
        if not initial_stop.startswith("T"):
            raise RuntimeError("GDB stub did not pause the game on connection")
        paused = True
        offset_reply = remote.command("qOffsets")
        base = runtime_main_base(log_path)
        report["main_base"] = hex(base)
        report["qOffsets"] = offset_reply
        for offset, signature in SIGNATURES.items():
            expected = SIGNATURE_ALTERNATES.get(offset, (signature,))
            matched = False
            for candidate in expected:
                try:
                    matched = remote.memory(base + offset, len(candidate)) == candidate
                except RuntimeError:
                    matched = False
                if matched:
                    break
            if not matched:
                raise RuntimeError(f"guest main mismatch at 0x{base + offset:x}")

        callback, commit, trap = base + CALLBACK, base + COMMIT, base + TRAP
        if mode == "direct":
            try:
                context = json.loads(CONTEXT_PATH.read_text(encoding="utf-8"))
                if (context.get("main_sha256") != MAIN_SHA256 or
                        int(context.get("main_base", "0"), 16) != base):
                    raise RuntimeError("saved inventory context belongs to another game session")
                if context.get("used", False):
                    raise RuntimeError("saved inventory context was already consumed; capture a new event")
                container = int(context["container"], 16)
                thread_id = int(context["thread"], 16)
            except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
                raise RuntimeError(f"inventory context is missing or invalid: {error}") from error
            if not container:
                raise RuntimeError("saved inventory container is null")
            if remote.command(f"Hc{thread_id:x}") != "OK":
                raise RuntimeError(f"captured inventory thread {thread_id:x} is no longer active")

            original = remote.command("g")
            regs = registers(original)
            stack = (regs["sp"] - 0x300) & ~0xF
            desc, hashed = stack + 0x40, stack + 0x50
            remote.write_memory(desc, bytes(8))
            remote.write_memory(hashed, struct.pack("<Q", item_hash(iid)))
            if remote.command(f"Z0,{trap:x},4") != "OK":
                raise RuntimeError("return trap breakpoint rejected")
            patched.append(trap)
            calls_started = True
            # A direct call is one-shot. Mark the cache before guest execution
            # so a failed call cannot be retried with a potentially stale stack.
            mark_inventory_context_used()
            guest_call(remote, base, LOOKUP, trap, desc, hashed, stack)
            descriptor = remote.memory(desc, 8)
            item_index = struct.unpack("<H", descriptor[:2])[0]
            report.update({"container": hex(container), "item_index": item_index,
                           "descriptor": descriptor.hex()})
            if item_index == 0:
                raise RuntimeError(f"game's Item table did not resolve {iid}")
            guest_call(remote, base, COMMIT, trap, container, desc, stack, quantity)
            report["injected_commit"] = True
            completed = True
            return report
        trigger = commit if mode == "commit" else callback
        prompt = (f"Waiting for one inventory commit at 0x{commit:x}; buy or pick up one item."
                  if mode == "commit" else
                  f"Waiting for one reward at 0x{callback:x}; open a chest or claim a scripted item.")
        print(prompt, flush=True)
        if remote.command(f"Z0,{trigger:x},4") != "OK":
            raise RuntimeError("reward callback breakpoint rejected")
        patched.append(trigger)
        report["stop"] = remote.continue_to_stop(wait)
        event_thread = stop_thread_id(report["stop"])
        paused = True
        original = remote.command("g")
        regs = registers(original)
        if regs["pc"] not in (trigger, trigger + 4):
            raise RuntimeError(f"stopped somewhere other than trigger: 0x{regs['pc']:x}")
        original = restore_at(original, trigger)

        if mode == "commit":
            stack = (regs["sp"] - 0x300) & ~0xF
            desc, hashed = stack + 0x40, stack + 0x50
            remote.write_memory(desc, bytes(8))
            remote.write_memory(hashed, struct.pack("<Q", item_hash(iid)))
            if remote.command(f"Z0,{trap:x},4") != "OK":
                raise RuntimeError("return trap breakpoint rejected")
            patched.append(trap)
            calls_started = True
            guest_call(remote, base, LOOKUP, trap, desc, hashed, stack)
            descriptor = remote.memory(desc, 8)
            item_index = struct.unpack("<H", descriptor[:2])[0]
            report["item_index"] = item_index
            report["descriptor"] = descriptor.hex()
            if item_index == 0:
                raise RuntimeError(f"game's Item table did not resolve {iid}")
            report["container"] = hex(regs["x0"])
            save_inventory_context(base, regs["x0"], event_thread)
            # In commit mode the trigger and the helper are the same entry.
            # Remove the event breakpoint before entering it re-entrantly;
            # otherwise execution stops at the entry again instead of trap.
            if remote.command(f"z0,{commit:x},4") != "OK":
                raise RuntimeError("inventory commit breakpoint removal failed")
            patched.remove(commit)
            # FUN_71008A25C0 ABI: x0=inventory container, x1=item descriptor,
            # w2=signed quantity.  The stopped event's x1 is the item being
            # purchased/picked up; use the resolved Vulnerary descriptor here.
            guest_call(remote, base, COMMIT, trap, regs["x0"], desc, stack, quantity)
            report["injected_commit"] = True
            completed = True
            return report
        owner = regs["x0"]
        raw_vector = remote.memory(owner + 0x98, 24)
        begin, end, capacity = struct.unpack("<QQQ", raw_vector)
        if not (0 < begin < end <= capacity and (end - begin) % 8 == 0
                and 0 < (end - begin) // 8 < 1024):
            raise RuntimeError(f"reward vector is empty or invalid: {raw_vector.hex()}")
        report["existing_reward_count"] = (end - begin) // 8
        report["existing_descriptors"] = remote.memory(begin, min(end - begin, 64)).hex()

        if mode == "grant":
            # Keep inputs above the new stack pointer; the callees use space below it.
            stack = (regs["sp"] - 0x300) & ~0xF
            desc, hashed = stack + 0x40, stack + 0x50
            remote.write_memory(desc, bytes(8))
            remote.write_memory(hashed, struct.pack("<Q", item_hash(iid)))
            if remote.command(f"Z0,{trap:x},4") != "OK":
                raise RuntimeError("return trap breakpoint rejected")
            patched.append(trap)
            calls_started = True
            guest_call(remote, base, LOOKUP, trap, desc, hashed, stack)
            descriptor = remote.memory(desc, 8)
            item_index = struct.unpack("<H", descriptor[:2])[0]
            report["item_index"] = item_index
            report["descriptor"] = descriptor.hex()
            if item_index == 0:
                raise RuntimeError(f"game's Item table did not resolve {iid}")
            guest_call(remote, base, APPEND, trap, owner + 0x98, desc, stack)
            new_begin, new_end, new_capacity = struct.unpack(
                "<QQQ", remote.memory(owner + 0x98, 24))
            if not (new_begin and new_end - new_begin == end - begin + 8
                    and new_end <= new_capacity
                    and remote.memory(new_end - 8, 8)[:2] == descriptor[:2]):
                raise RuntimeError("reward vector append did not produce one item")
            report["appended"] = True
        completed = True
        return report
    finally:
        if paused:
            if original is not None:
                if remote.command("G" + original) != "OK":
                    raise RuntimeError("failed to restore guest CPU registers")
            for address in reversed(patched):
                if remote.command(f"z0,{address:x},4") != "OK":
                    raise RuntimeError(f"failed to remove breakpoint 0x{address:x}")
            if completed or not calls_started:
                remote.resume()
            else:
                print("Guest helper stopped unexpectedly; game left paused for inspection.", flush=True)
        remote.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("observe", "grant", "commit", "direct"), default="observe")
    parser.add_argument("--port", type=int, default=6543)
    parser.add_argument("--wait", type=int, default=180, help="seconds to await the reward event")
    parser.add_argument("--iid", default=IID, help="item IID, for example IID_傷薬")
    parser.add_argument("--quantity", type=int, default=1,
                        help="positive quantity for commit mode (1..999)")
    parser.add_argument("--main", type=Path, default=MAIN,
                        help="local NSO path used for the optional build check")
    parser.add_argument("--log", type=Path, default=EDEN_LOG,
                        help="Eglantine log path")
    parser.add_argument("--report-dir", type=Path, default=PACKAGE_ROOT / "reports",
                        help="directory for JSON operation reports")
    parser.add_argument("--skip-local-main-check", action="store_true",
                        help="rely on runtime signatures instead of reading a local NSO")
    options = parser.parse_args()
    target = options.report_dir / f"itemadd_{datetime.now():%Y%m%d_%H%M%S}.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = run(options.mode, options.port, options.wait, options.iid, options.quantity,
                     options.main, options.log, options.skip_local_main_check)
    except Exception as error:
        result = {"iid": options.iid, "quantity": options.quantity, "mode": options.mode,
                  "error": f"{type(error).__name__}: {error}"}
        target.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"report": str(target), **result}, ensure_ascii=False, indent=2))
        raise
    target.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(target), **result}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
