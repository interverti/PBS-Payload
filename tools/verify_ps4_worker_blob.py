#!/usr/bin/env python3
"""Reject payload references that would break a relocated PS4 worker blob."""
import re
import subprocess
import sys


def verify(path):
    symbols = subprocess.check_output(["nm", "-n", path], text=True)
    found = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) == 3:
            found[parts[2]] = int(parts[0], 16)
    start = found["__start_pbs_worker_remote"]
    stop = found["__stop_pbs_worker_remote"]
    if found["pbs_worker_remote_main"] != start or not 32 <= stop - start <= 0x1000:
        raise ValueError("invalid worker entry or blob size")
    disasm = subprocess.check_output(
        ["objdump", "-d", "--no-show-raw-insn", "-j", "pbs_worker_remote", path],
        text=True,
    )
    for line in disasm.splitlines():
        match = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)", line)
        if not match:
            continue
        _, op, operands = match.groups()
        if "%rip" in operands:
            raise ValueError("worker references payload data: " + line.strip())
        if op.startswith(("call", "j", "loop")) and not operands.startswith("*"):
            target = re.match(r"([0-9a-f]+)\b", operands)
            if not target or not start <= int(target[1], 16) < stop:
                raise ValueError("worker branches outside its blob: " + line.strip())
    print(f"worker blob: {stop - start} bytes, entry at start, no external code/data references")


if __name__ == "__main__":
    verify(sys.argv[1])
