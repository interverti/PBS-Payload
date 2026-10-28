#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a PS4 offset dump into a firmware-profile skeleton.

Input is the report written by the `dump` payload build (dump.txt or
offsets.txt, both accepted). Nothing here contacts a console. With --module
the reconstructed libSceAvcap ELF from the `probe` payload is scanned for the
16-byte import-stub/padding runs the trampoline borrows, and the candidates
are reported next to the measured avcap_init_offset.

The output is deliberately incomplete: the kernel gate bytes and the export
RVAs are measured, while the ring deltas and the trap slot still need a
reviewed comparison against a known profile.
"""
import argparse
import json
import re
import struct
from pathlib import Path

HEADER = re.compile(r'^#?\s*PS4-DUMP v1 firmware=(0x[0-9a-fA-F]+)')
OFFSET = re.compile(r'^([a-z0-9_]+)=(0x[0-9a-fA-F]+|unresolved)$')
SYMBOL = re.compile(
    r'^symbol process=(\S+) module=(\S+) label=(\S+) '
    r'address=(0x[0-9a-fA-F]+) rva=(0x[0-9a-fA-F]+|unresolved)$')
KERNEL = re.compile(
    r'^kernel layout status=(discovered|unavailable)')
KERNEL_FIELDS = {
    'acmgr': r'acmgr=(0x[0-9a-fA-F]+)',
    'gate0': r'gate0=(0x[0-9a-fA-F]+)',
    'before0': r'before0=(\S+)',
    'after0': r'after0=(\S+)',
    'size0': r'size0=(\d+)',
    'gate1': r'gate1=(0x[0-9a-fA-F]+)',
    'before1': r'before1=(\S+)',
    'after1': r'after1=(\S+)',
    'size1': r'size1=(\d+)',
}


def parse_report(text):
    """Parse dump.txt or offsets.txt into firmware, offsets and symbols."""
    firmware = None
    offsets = {}
    symbols = []
    kernel = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        match = HEADER.match(line)
        if match:
            firmware = int(match.group(1), 16)
            continue
        if line.startswith('#'):
            continue
        match = SYMBOL.match(line)
        if match:
            rva = match.group(5)
            symbols.append({
                'process': match.group(1), 'module': match.group(2),
                'label': match.group(3), 'address': int(match.group(4), 16),
                'rva': None if rva == 'unresolved' else int(rva, 16),
            })
            continue
        match = KERNEL.match(line)
        if match:
            kernel = {'status': match.group(1)}
            for key, pattern in KERNEL_FIELDS.items():
                found = re.search(pattern, line)
                if not found:
                    continue
                value = found.group(1)
                if key.startswith('size'):
                    kernel[key] = int(value)
                elif key.startswith(('before', 'after')):
                    kernel[key] = [int(b, 16) for b in value.split(',') if b.startswith('0x')]
                else:
                    kernel[key] = int(value, 16)
            continue
        match = OFFSET.match(line)
        if match:
            value = match.group(2)
            offsets[match.group(1)] = None if value == 'unresolved' else int(value, 16)
    return firmware, kernel, offsets, symbols


def elf_text_segments(data):
    """Yield (file_offset, size, vaddr) for executable ELF64 LOAD segments."""
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected a little-endian ELF64 module dump')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    for index in range(count):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from(
            '<IIQQQQQQ', data, phoff + index * stride)
        if p_type == 1 and p_flags & 1 and p_filesz:
            yield p_offset, p_filesz, p_vaddr


def import_padding_runs(data):
    """Find runs of FF25+rel32 stubs each followed by ten 0xCC padding bytes.

    Mirrors pbs_ps4_layout_trap(): the stub is six bytes, the padding starts
    at +6, the stride is sixteen and a usable run has twenty slots. Returns
    (rva, count) pairs using the segment virtual addresses.
    """
    result = []
    for offset, size, vaddr in elf_text_segments(data):
        segment = data[offset:offset + size]
        stubs = []
        index = 0
        while index + 16 <= len(segment):
            if segment[index] == 0xFF and segment[index + 1] == 0x25 and \
                    segment[index + 6:index + 16] == b'\xcc' * 10:
                stubs.append(index)
                index += 16
            else:
                index += 1
        completed = []
        run = None
        for stub in stubs:
            if run is not None and stub == run['end']:
                run['end'] = stub + 16
                run['count'] += 1
                continue
            if run is not None and run['count'] >= 20:
                completed.append((vaddr + run['start'], run['count']))
            run = {'start': stub, 'end': stub + 16, 'count': 1}
        if run is not None and run['count'] >= 20:
            completed.append((vaddr + run['start'], run['count']))
        result.extend(completed)
    return sorted(result)


def nearest_run(runs, init_rva):
    """Candidate run for the trampoline: nearest run start before the export."""
    if init_rva is None or not runs:
        return None
    before = [run for run in runs if run[0] < init_rva and init_rva - run[0] <= 0x10000]
    return max(before, key=lambda run: run[0]) if before else None


def snippet(firmware, kernel, offsets, runs, init_rva):
    lines = ['  .name = "PS4 %08x / dumped on console",' % (firmware or 0),
             '  .version = 0x%08xu, .version_mask = 0xffff0000u,' % (firmware or 0)]
    if kernel and kernel.get('status') == 'discovered':
        def byte_list(values):
            return ', '.join('0x%02x' % value for value in values)
        lines.append('  .acmgr_offset = 0x%x,' % kernel['acmgr'])
        lines.append('  .acmgr_signature = {%s},' % ', '.join(
            '0x%02x' % byte for byte in (0xb0, 1, 0xc3)))
        lines.append('  .gates = {')
        lines.append('    {0x%x, {%s}, {%s}, %d},' % (
            kernel['gate0'], byte_list(kernel['before0']),
            byte_list(kernel['after0']), kernel['size0']))
        lines.append('    {0x%x, {%s}, {%s}, %d}' % (
            kernel['gate1'], byte_list(kernel['before1']),
            byte_list(kernel['after1']), kernel['size1']))
        lines.append('  },')
    else:
        lines.append('  /* TODO kernel layout: dump reported unavailable. */')
    if init_rva is not None:
        lines.append('  .avcap_init_offset = 0x%x,' % init_rva)
    else:
        lines.append('  /* TODO avcap_init_offset: avcap_init unresolved. */')
    candidate = nearest_run(runs, init_rva)
    if candidate:
        lines.append('  /* Import-padding run starts at module RVA 0x%x (%d slots);'
                     ' the known 11.00 profile places the trap 0xbe bytes into'
                     ' the first slot. */' % candidate)
    lines.append('  /* TODO avcap_trap_offset, ready_delta, dequeue_delta,'
                 ' consume_delta: derive from the module dump and review. */')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path, help='dump.txt or offsets.txt')
    parser.add_argument('--module', type=Path,
                        help='reconstructed libSceAvcap ELF from the probe payload')
    parser.add_argument('--json', type=Path, help='write the parsed data as JSON')
    parser.add_argument('--snippet', type=Path, help='write the C initializer')
    args = parser.parse_args()
    try:
        text = args.report.read_text(encoding='utf-8', errors='replace')
    except OSError as error:
        raise SystemExit('cannot read %s: %s' % (args.report, error))
    firmware, kernel, offsets, symbols = parse_report(text)
    if firmware is None:
        raise SystemExit('no PS4-DUMP header found in %s' % args.report)
    init_rva = offsets.get('avcap_init')
    for symbol in symbols:
        if symbol['label'] == 'avcap_init' and symbol['rva'] is not None:
            init_rva = symbol['rva']
    runs = []
    if args.module:
        try:
            runs = import_padding_runs(args.module.read_bytes())
        except OSError as error:
            raise SystemExit('cannot read %s: %s' % (args.module, error))
        except ValueError as error:
            raise SystemExit('%s: %s' % (args.module, error))
    data = {
        'firmware': '0x%08x' % firmware,
        'kernel': kernel,
        'offsets': {key: ('0x%x' % value) if value is not None else 'unresolved'
                    for key, value in sorted(offsets.items())},
        'symbols': symbols,
        'import_padding_runs': [{'rva': '0x%x' % rva, 'slots': count}
                                for rva, count in runs],
        'trap_candidate': ('0x%x' % nearest_run(runs, init_rva)[0])
                          if nearest_run(runs, init_rva) else None,
    }
    text = snippet(firmware, kernel, offsets, runs, init_rva)
    if args.json:
        args.json.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    if args.snippet:
        args.snippet.write_text(text, encoding='utf-8')
    print(json.dumps({key: data[key] for key in
                      ('firmware', 'trap_candidate', 'import_padding_runs')},
                     indent=2))
    print(text)


if __name__ == '__main__':
    main()
