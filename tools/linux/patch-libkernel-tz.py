#!/usr/bin/env python3
"""Lets a RyGT build that converts guest local time with Windows' time-zone rules start under Proton.

Builds from the development branch since 2026-10-18 (commit 7855fd6, "Use Windows system timezone
for guest local clock conversions") import SystemTimeToTzSpecificLocalTimeEx and
TzSpecificLocalTimeToSystemTimeEx in libs/libkernel.prx. Wine only has stubs for them, and the
game aborts at the main menu ("unimplemented function KERNEL32.dll.SystemTimeToTzSpecificLocalTimeEx"
in the Proton output). This points the two imports at the versions without Ex, which Wine has:
the zone passed is a DYNAMIC_TIME_ZONE_INFORMATION from GetDynamicTimeZoneInformation, whose
leading fields are a TIME_ZONE_INFORMATION holding the current rules, so they get the same input.

Usage: patch-libkernel-tz.py <RyGT folder>   (keeps libs/libkernel.prx.orig; safe to run again)
SPDX-License-Identifier: GPL-2.0-only
"""
import pathlib
import shutil
import struct
import sys

NAMES = (b"SystemTimeToTzSpecificLocalTimeEx", b"TzSpecificLocalTimeToSystemTimeEx")


def import_names(data: bytes) -> dict:
    """Hint/name entries of the PE import table, by name: {name: offset of the name}."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    import_rva = struct.unpack_from("<I", data, pe + 24 + 120)[0]  # PE32+ data directory 1
    table = pe + 24 + optional

    def offset(rva):
        for i in range(sections):
            # VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData
            virtual_size, va, raw_size, raw = struct.unpack_from("<IIII", data, table + i * 40 + 8)
            if va <= rva < va + max(virtual_size, raw_size):
                return rva - va + raw
        raise ValueError(f"RVA {rva:#x} is in no section")

    found = {}
    descriptor = offset(import_rva)
    while True:
        lookup = struct.unpack_from("<I", data, descriptor)[0]
        if lookup == 0:
            break
        entry = offset(lookup)
        while (thunk := struct.unpack_from("<Q", data, entry)[0]) != 0:
            if not thunk & (1 << 63):
                name = offset(thunk & 0x7FFFFFFF) + 2
                found[bytes(data[name:data.index(b"\0", name)])] = name
            entry += 8
        descriptor += 20
    return found


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-2], file=sys.stderr)
        return 2
    library = pathlib.Path(sys.argv[1]) / "libs" / "libkernel.prx"
    data = bytearray(library.read_bytes())
    names = import_names(data)
    todo = [n for n in NAMES if n in names]
    if not todo:
        done = all(n[:-2] in names for n in NAMES)
        print(f"{library}: {'already patched' if done else 'does not import the Ex functions'}")
        return 0
    backup = library.with_name(library.name + ".orig")
    if not backup.exists():
        shutil.copy2(library, backup)
    for name in todo:
        end = names[name] + len(name)
        data[end - 2:end] = b"\0\0"  # "...Ex\0" -> "...\0\0\0"
        print(f"{library}: {name.decode()} -> {name[:-2].decode()}")
    library.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
