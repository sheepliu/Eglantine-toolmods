#!/usr/bin/env python3
"""Read-only relationship-object scanner for one or more normal gifts.

This helper never changes x1, guest memory, saves, or the game's return path.
It records the reaction arguments and GameReliance::IData pointer for each gift,
then performs a bounded read-only probe of IData and its two related pointers.
The pointer windows are used to locate runtime MPID strings or object links
without assuming that IData's small type byte is a character ID.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ITEMADD = ROOT / "work" / "mods" / "ItemAdd_Runtime"
sys.path.insert(0, str(ITEMADD))

from itemadd_gdb import (  # noqa: E402
    BUILD_ID,
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


def read_window(remote: Remote, pointer: int, size: int = 0x40) -> str | None:
    if pointer < 0x100000000:
        return None


def ascii_strings(raw: bytes, minimum: int = 4) -> list[str]:
    values: list[str] = []
    start: int | None = None
    for index, byte in enumerate(raw + b"\0"):
        printable = 0x20 <= byte < 0x7F
        if printable and start is None:
            start = index
        elif not printable and start is not None:
            if index - start >= minimum:
                values.append(raw[start:index].decode("ascii", errors="replace"))
            start = None
    return values


def pointer_probe(remote: Remote, pointer: int, size: int = 0x200) -> dict:
    if pointer < 0x100000000:
        return {"pointer": hex(pointer), "valid": False}
    try:
        raw = remote.memory(pointer, size)
        words = [struct.unpack_from("<Q", raw, offset)[0]
                 for offset in range(0, len(raw) - 7, 8)]
        heap_pointers = [hex(value) for value in words
                         if 0x100000000 <= value < 0x20000000000]
        strings = ascii_strings(raw)
        return {"pointer": hex(pointer), "valid": True,
                "window": raw.hex(), "ascii": strings[:80],
                "mpid_strings": [value for value in strings if "MPID_" in value],
                "heap_pointers": heap_pointers[:80]}
    except Exception as error:
        return {"pointer": hex(pointer), "valid": False,
                "error": f"{type(error).__name__}: {error}"}
    try:
        return remote.memory(pointer, size).hex()
    except Exception:
        return None


def idata_snapshot(remote: Remote, pointer: int) -> dict:
    result: dict = {"pointer": hex(pointer), "window": read_window(remote, pointer)}
    try:
        result.update({
            "type": remote.memory(pointer + 0x0A, 1)[0],
            "level": remote.memory(pointer + 0x0B, 1)[0],
            "score": struct.unpack("<H", remote.memory(pointer + 0x0C, 2))[0],
            "related_18": hex(struct.unpack("<Q", remote.memory(pointer + 0x18, 8))[0]),
            "related_20": hex(struct.unpack("<Q", remote.memory(pointer + 0x20, 8))[0]),
        })
        result["pointer_graph"] = {
            "idata": pointer_probe(remote, pointer),
            "related_18": pointer_probe(remote, struct.unpack("<Q", remote.memory(pointer + 0x18, 8))[0]),
            "related_20": pointer_probe(remote, struct.unpack("<Q", remote.memory(pointer + 0x20, 8))[0]),
        }
    except Exception as error:
        result["field_error"] = f"{type(error).__name__}: {error}"
    return result


def check_local_main(path: Path) -> None:
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != MAIN_SHA256 or data[0x40:0x60].hex() != BUILD_ID:
        raise RuntimeError("local main is not the analyzed Fortune's Weave build")


def wait_entry(remote: Remote, address: int, timeout: int) -> tuple[str, str, dict[str, int]]:
    if remote.command(f"Z0,{address:x},4") != "OK":
        raise RuntimeError(f"breakpoint rejected at 0x{address:x}")
    stop = remote.continue_to_stop(timeout)
    packet = remote.command("g")
    regs = registers(packet)
    if regs["pc"] not in (address, address + 4):
        raise RuntimeError(f"stopped at 0x{regs['pc']:x}, expected 0x{address:x}")
    return stop, packet, regs


def run(count: int, port: int, wait: int, main_path: Path, log_path: Path) -> dict:
    check_local_main(main_path)
    remote = Remote("127.0.0.1", port)
    paused = False
    breakpoints: list[int] = []
    try:
        initial = remote.command("?")
        if not initial.startswith("T"):
            raise RuntimeError(f"GDB stub did not stop on connection: {initial}")
        paused = True
        base = runtime_main_base(log_path)
        for offset in (REACTION, SCORE_WRITER):
            if remote.memory(base + offset, 4) != PROLOGUE:
                raise RuntimeError(f"guest main mismatch at 0x{base + offset:x}")
        reaction = base + REACTION
        writer = base + SCORE_WRITER
        report = {
            "main_sha256": MAIN_SHA256,
            "build_id": BUILD_ID,
            "main_base": hex(base),
            "reaction": hex(reaction),
            "score_writer": hex(writer),
            "events": [],
        }

        for index in range(count):
            reaction_stop, reaction_packet, reaction_regs = wait_entry(remote, reaction, wait)
            breakpoints.append(reaction)
            reaction_thread = stop_thread_id(reaction_stop)
            if reaction_thread is None:
                raise RuntimeError("reaction stop did not include a guest thread")
            event = {
                "sequence": index + 1,
                "reaction_stop": reaction_stop,
                "reaction_thread": hex(reaction_thread),
                "selected_item_index": struct.unpack(
                    "<I", remote.memory(base + SELECTED_ITEM, 4))[0],
                "reaction_regs": {key: hex(value) for key, value in reaction_regs.items()},
                "reaction_x0_window": read_window(remote, reaction_regs["x0"], 0x100),
                "reaction_x1_window": read_window(remote, reaction_regs["x1"], 0x100),
            }
            remote.command(f"z0,{reaction:x},4")
            breakpoints.remove(reaction)
            remote.command("G" + restore_at(reaction_packet, reaction))
            remote.command(f"Hc{reaction_thread:x}")

            score_stop, score_packet, score_regs = wait_entry(remote, writer, wait)
            breakpoints.append(writer)
            if stop_thread_id(score_stop) != reaction_thread:
                raise RuntimeError("score writer stopped on an unexpected thread")
            event.update({
                "score_stop": score_stop,
                "score_regs": {key: hex(value) for key, value in score_regs.items()},
                "idata": idata_snapshot(remote, score_regs["x0"]),
            })
            return_pc = score_regs["x30"]
            if return_pc & 3:
                raise RuntimeError(f"unaligned return address 0x{return_pc:x}")
            remote.command(f"z0,{writer:x},4")
            breakpoints.remove(writer)
            remote.command(f"Z0,{return_pc:x},4")
            breakpoints.append(return_pc)
            remote.command("G" + restore_at(score_packet, writer))
            return_stop = remote.continue_to_stop(wait)
            return_packet = remote.command("g")
            return_regs = registers(return_packet)
            if return_regs["pc"] not in (return_pc, return_pc + 4):
                raise RuntimeError(f"writer returned at 0x{return_regs['pc']:x}")
            event["return_stop"] = return_stop
            event["idata_after"] = idata_snapshot(remote, score_regs["x0"])
            remote.command(f"z0,{return_pc:x},4")
            breakpoints.remove(return_pc)
            remote.command("G" + restore_at(return_packet, return_pc))
            report["events"].append(event)
        report["completed"] = True
        return report
    finally:
        if paused:
            for address in reversed(breakpoints):
                try:
                    remote.command(f"z0,{address:x},4")
                except Exception:
                    pass
            try:
                remote.resume()
            except Exception:
                pass
        remote.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=1, help="number of gifts to capture")
    parser.add_argument("--port", type=int, default=6543)
    parser.add_argument("--wait", type=int, default=600)
    parser.add_argument("--main", type=Path, default=ROOT / "exefs" / "main")
    parser.add_argument("--log", type=Path, default=EDEN_LOG)
    parser.add_argument("--report-dir", type=Path, default=ROOT / "work" / "friendship_probe")
    options = parser.parse_args()
    if options.count < 1 or options.count > 20:
        raise SystemExit("--count must be between 1 and 20")
    report_path = options.report_dir / f"friendship_scan_{datetime.now():%Y%m%d_%H%M%S}.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    result = run(options.count, options.port, options.wait, options.main, options.log)
    report_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(report_path), **result}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
