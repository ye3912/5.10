#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Minimal mkdtboimg.py — create a standard Android dtbo.img from .dtbo files.
# Derived from AOSP system/libufdt/utils/src/mkdtboimg.py
#
# Usage:
#   mkdtboimg.py create <output> <input.dtbo> [input2.dtbo ...]

import struct
import sys
import os

DT_TABLE_MAGIC = 0xD7B7AB1E


class DtTableHeader:
    """struct dt_table_header — 8 x uint32_t, big‑endian."""

    fmt = ">8I"

    def __init__(self, magic=DT_TABLE_MAGIC):
        self.magic = magic
        self.total_size = 0
        self.header_size = 0
        self.dt_entry_size = 0
        self.dt_entry_count = 0
        self.dt_entries_offset = 0
        self.page_size = 4096
        self.version = 0

    def pack(self):
        return struct.pack(
            self.fmt,
            self.magic,
            self.total_size,
            self.header_size,
            self.dt_entry_size,
            self.dt_entry_count,
            self.dt_entries_offset,
            self.page_size,
            self.version,
        )


class DtTableEntry:
    """struct dt_table_entry — dt_size, dt_offset, id, rev, custom[4]."""

    fmt = ">8I"

    def __init__(self):
        self.dt_size = 0
        self.dt_offset = 0
        self.id = 0
        self.rev = 0
        self.custom = (0, 0, 0, 0)

    def pack(self):
        return struct.pack(
            self.fmt,
            self.dt_size,
            self.dt_offset,
            self.id,
            self.rev,
            self.custom[0],
            self.custom[1],
            self.custom[2],
            self.custom[3],
        )


def create(output_path, input_paths):
    entries = []
    payloads = []

    for path in input_paths:
        with open(path, "rb") as f:
            data = f.read()
        entry = DtTableEntry()
        entry.dt_size = len(data)
        entries.append(entry)
        payloads.append(data)

    hdr = DtTableHeader()
    hdr.dt_entry_count = len(entries)
    hdr.dt_entry_size = struct.calcsize(DtTableEntry.fmt)  # 32
    hdr.dt_entries_offset = struct.calcsize(DtTableHeader.fmt)  # 32
    hdr.header_size = struct.calcsize(DtTableHeader.fmt)  # 32

    # Layout: header(32) | entry_hdr[0](32) | entry_hdr[1](32) | ... | payload[0] | payload[1] | ...
    metadata_size = hdr.header_size + hdr.dt_entry_count * hdr.dt_entry_size
    dt_offset = metadata_size
    for entry, data in zip(entries, payloads):
        entry.dt_offset = dt_offset
        dt_offset += entry.dt_size

    hdr.total_size = metadata_size + sum(e.dt_size for e in entries)

    with open(output_path, "wb") as out:
        out.write(hdr.pack())
        for entry in entries:
            out.write(entry.pack())
        for data in payloads:
            out.write(data)

    print(
        f"Created {output_path} ({hdr.dt_entry_count} entries, "
        f"total {hdr.total_size} bytes)"
    )


def usage():
    name = os.path.basename(sys.argv[0])
    print(f"Usage: {name} create <output> <input.dtbo> [input2.dtbo ...]",
          file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) < 4:
        usage()
    cmd = sys.argv[1]
    if cmd != "create":
        usage()
    create(sys.argv[2], sys.argv[3:])
