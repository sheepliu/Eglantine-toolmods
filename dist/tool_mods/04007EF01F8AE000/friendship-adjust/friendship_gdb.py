#!/usr/bin/env python3
"""Apply a configured relationship-point delta to one normal gift.

The script waits for the game's gift-result path, then arms the game's own
GameReliance::IData score writer on the same guest thread. At that function's
entry it changes only x1 (the signed point delta) and lets the original code
run. It does not call a guest function from the host thread and does not write
guest memory or save data directly.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
# The package carries its own GDB protocol helper so it remains installable
# without the development workspace or the ItemAdd package.
PACKAGE_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(PACKAGE_ROOT))

from itemadd_gdb import (  # noqa: E402
    BUILD_ID,
    CancelledError,
    EDEN_LOG,
    MAIN_SHA256,
    Remote,
    registers,
    restore_at,
    runtime_main_base,
    stop_thread_id,
)


REACTION = 0x10260E0
SCORE_WRITER = 0x8EC500
SELECTED_ITEM = 0x21A7728
PROLOGUE = bytes.fromhex("7f2303d5")
MAX_WAIT = 600


def u8(remote: Remote, address: int) -> int:
    return remote.memory(address, 1)[0]


def u16(remote: Remote, address: int) -> int:
    return struct.unpack("<H", remote.memory(address, 2))[0]


def u32(remote: Remote, address: int) -> int:
    return struct.unpack("<I", remote.memory(address, 4))[0]


def u64(remote: Remote, address: int) -> int:
    return struct.unpack("<Q", remote.memory(address, 8))[0]


def snapshot(remote: Remote, pointer: int) -> dict[str, int | str]:
    return {
        "pointer": hex(pointer),
        "type": u8(remote, pointer + 0x0A),
        "level": u8(remote, pointer + 0x0B),
        "score": u16(remote, pointer + 0x0C),
        "related_18": hex(u64(remote, pointer + 0x18)),
        "related_20": hex(u64(remote, pointer + 0x20)),
    }


def set_packet_x1_pc(packet: str, value: int, pc: int) -> str:
    raw = bytearray.fromhex(packet)
    struct.pack_into("<Q", raw, 8, value & 0xFFFFFFFFFFFFFFFF)
    struct.pack_into("<Q", raw, 29 * 8 + 24, pc)
    return raw.hex()


def check_local_main(path: Path) -> None:
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != MAIN_SHA256 or data[0x40:0x60].hex() != BUILD_ID:
        raise RuntimeError("local main is not the analyzed Fortune's Weave build")


def runtime_check(remote: Remote, base: int, main_path: Path, skip: bool) -> None:
    if not skip:
        check_local_main(main_path)
    for offset in (REACTION, SCORE_WRITER):
        actual = remote.memory(base + offset, 4)
        if actual != PROLOGUE:
            raise RuntimeError(
                f"guest main mismatch at 0x{base + offset:x}: "
                f"expected {PROLOGUE.hex()}, got {actual.hex()}"
            )


def wait_entry(remote: Remote, address: int, timeout: int) -> tuple[str, str, dict[str, int]]:
    if remote.command(f"Z0,{address:x},4") != "OK":
        raise RuntimeError(f"breakpoint rejected at 0x{address:x}")
    stop = remote.continue_to_stop(timeout)
    raw = remote.command("g")
    regs = registers(raw)
    if regs["pc"] not in (address, address + 4):
        raise RuntimeError(f"stopped at 0x{regs['pc']:x}, expected 0x{address:x}")
    return stop, raw, regs


def run(amount: int, port: int, wait: int, main_path: Path, log_path: Path,
        skip_local_main_check: bool, cancel_file: Path | None = None) -> dict:
    if not 1 <= amount <= 50000:
        raise ValueError("amount must be between 1 and 50000")
    remote = Remote("127.0.0.1", port, cancel_file)
    paused = False
    breakpoints: list[int] = []
    try:
        initial = remote.command("?")
        if not initial.startswith("T"):
            raise RuntimeError(f"GDB stub did not stop on connection: {initial}")
        paused = True
        base = runtime_main_base(log_path)
        runtime_check(remote, base, main_path, skip_local_main_check)
        reaction = base + REACTION
        writer = base + SCORE_WRITER
        report: dict = {
            "amount_requested": amount,
            "main_sha256": MAIN_SHA256,
            "build_id": BUILD_ID,
            "main_base": hex(base),
            "qOffsets": remote.command("qOffsets"),
            "reaction": hex(reaction),
            "score_writer": hex(writer),
        }

        breakpoints.append(reaction)
        reaction_stop, reaction_packet, reaction_regs = wait_entry(remote, reaction, wait)
        reaction_thread = stop_thread_id(reaction_stop)
        report.update({
            "reaction_stop": reaction_stop,
            "reaction_thread": hex(reaction_thread or 0),
            "selected_item_index": u32(remote, base + SELECTED_ITEM),
        })
        if reaction_thread is None:
            raise RuntimeError("gift reaction stop did not include a guest thread")

        if remote.command(f"z0,{reaction:x},4") != "OK":
            raise RuntimeError("failed to remove gift reaction breakpoint")
        breakpoints.remove(reaction)
        if remote.command("G" + restore_at(reaction_packet, reaction)) != "OK":
            raise RuntimeError("failed to restore gift reaction registers")
        if remote.command(f"Hc{reaction_thread:x}") != "OK":
            raise RuntimeError(f"gift thread {reaction_thread:x} is no longer active")

        breakpoints.append(writer)
        score_stop, score_packet, score_regs = wait_entry(remote, writer, wait)
        if stop_thread_id(score_stop) != reaction_thread:
            raise RuntimeError(
                f"score writer stopped on unexpected thread {stop_thread_id(score_stop)}"
            )
        pointer = score_regs["x0"]
        before = snapshot(remote, pointer)
        original_amount = score_regs["x1"] & 0xFFFFFFFF
        report.update({
            "score_stop": score_stop,
            "score_thread": hex(reaction_thread),
            "original_amount": original_amount,
            "score_mode": score_regs["x2"] & 0xFF,
            "before": before,
        })

        return_pc = score_regs["x30"]
        if return_pc & 3:
            raise RuntimeError(f"unaligned score-writer return address 0x{return_pc:x}")
        if remote.command(f"z0,{writer:x},4") != "OK":
            raise RuntimeError("failed to remove score-writer breakpoint")
        breakpoints.remove(writer)
        if remote.command(f"Z0,{return_pc:x},4") != "OK":
            raise RuntimeError(f"return breakpoint rejected at 0x{return_pc:x}")
        breakpoints.append(return_pc)
        # Re-enter the game's own writer with only x1 changed. The breakpoint
        # is gone, so the original PACIBSP and all following instructions run.
        changed_packet = set_packet_x1_pc(score_packet, amount, writer)
        if remote.command("G" + changed_packet) != "OK":
            raise RuntimeError("failed to set adjusted score-writer registers")
        adjusted_stop = remote.continue_to_stop(wait)
        adjusted_packet = remote.command("g")
        adjusted_regs = registers(adjusted_packet)
        if adjusted_regs["pc"] not in (return_pc, return_pc + 4):
            raise RuntimeError(f"score writer returned at 0x{adjusted_regs['pc']:x}")
        report["adjusted_stop"] = adjusted_stop
        report["after"] = snapshot(remote, pointer)
        report["score_delta_observed"] = report["after"]["score"] - report["before"]["score"]

        if remote.command(f"z0,{return_pc:x},4") != "OK":
            raise RuntimeError("failed to remove score return breakpoint")
        breakpoints.remove(return_pc)
        if remote.command("G" + restore_at(adjusted_packet, return_pc)) != "OK":
            raise RuntimeError("failed to restore score return registers")
        report["completed"] = True
        return report
    finally:
        if paused:
            restore_packet = None
            restore_pc = None
            try:
                current_packet = remote.command("g")
                current_pc = registers(current_packet)["pc"]
                if current_pc in breakpoints:
                    restore_packet = current_packet
                    restore_pc = current_pc
                elif (current_pc - 4) in breakpoints:
                    restore_packet = current_packet
                    restore_pc = current_pc - 4
            except Exception:
                pass
            for address in reversed(breakpoints):
                try:
                    remote.command(f"z0,{address:x},4")
                except Exception:
                    pass
            if restore_packet is not None and restore_pc is not None:
                try:
                    remote.command("G" + restore_at(restore_packet, restore_pc))
                except Exception:
                    pass
            try:
                remote.resume()
            except Exception:
                pass
        remote.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--amount", type=int, default=500,
                        help="replacement relationship-point delta (1..50000)")
    parser.add_argument("--port", type=int, default=6543)
    parser.add_argument("--wait", type=int, default=MAX_WAIT)
    parser.add_argument("--main", type=Path, default=ROOT / "exefs" / "main")
    parser.add_argument("--log", type=Path, default=EDEN_LOG)
    parser.add_argument("--report-dir", type=Path, default=ROOT / "work" / "friendship_probe")
    parser.add_argument("--skip-local-main-check", action="store_true")
    parser.add_argument("--cancel-file", type=Path)
    options = parser.parse_args()
    report_path = options.report_dir / f"friendship_{datetime.now():%Y%m%d_%H%M%S}.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = run(options.amount, options.port, options.wait, options.main, options.log,
                     options.skip_local_main_check, options.cancel_file)
    except CancelledError as error:
        result = {"amount_requested": options.amount, "cancelled": True,
                  "message": str(error)}
        report_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
        print(json.dumps({"report": str(report_path), **result}, ensure_ascii=False, indent=2))
        return
    except Exception as error:
        result = {"amount_requested": options.amount,
                  "error": f"{type(error).__name__}: {error}"}
        report_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
        print(json.dumps({"report": str(report_path), **result}, ensure_ascii=False, indent=2))
        raise
    report_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    print(json.dumps({"report": str(report_path), **result}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
