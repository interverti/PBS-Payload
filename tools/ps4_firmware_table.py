#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate, compare and render the per-firmware PS4 offset table.

The table lives in tools/data/ps4_firmware_offsets.json: one entry per firmware
release, values as module- or kernel-relative RVAs, null when the firmware was
never measured (never zero, which would look like a real offset). This tool is
the single place that reads it, so the documentation and the C profile cannot
drift apart:

  --list              every firmware, every measured field
  --markdown          the tables used in docs/PS4_FIRMWARE_OFFSETS.txt
  --compare 9.00 11.00  field-by-field differences and deltas
  --report dump.txt --firmware 9.00   cross-check a dump payload report
  --c 9.00            paste-ready pbs_ps4_firmware initializer
  --check             schema and consistency validation

Nothing here contacts a console.
"""
import argparse
import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from ps4_offsets import parse_report  # noqa: E402  (same directory)

DEFAULT_TABLE = TOOLS / 'data' / 'ps4_firmware_offsets.json'
SECTIONS = ('kernel', 'avcap', 'libraries', 'remoteplay', 'capture')
# Report label -> table path. The dump payload uses avcap_* labels.
LABEL_PATHS = {
    'avcap_init': 'avcap.init',
    'avcap_trap': 'avcap.trap',
    'avcap_module_size': 'avcap.module_size',
}


def die(message):
    raise SystemExit(message)


def as_hex(value, path):
    """Parse a 0x-prefixed string, or accept an int; None stays None."""
    if value is None:
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.lower().startswith('0x'):
        try:
            return int(value, 16)
        except ValueError:
            pass
    die('%s: expected a 0x-prefixed hex string, got %r' % (path, value))


METADATA = ('notes', 'evidence', 'trap_evidence', 'release', 'module',
            'measured_on', 'version', 'version_mask')


def flatten(node, prefix=''):
    """Yield (path, value) for every leaf under node, skipping prose fields."""
    if isinstance(node, dict) and 'offset' in node:
        # A patch site (offset + bytes) is one value, not four rows.
        yield prefix, node
    elif isinstance(node, dict):
        for key, value in node.items():
            if key in METADATA:
                continue
            yield from flatten(value, '%s.%s' % (prefix, key) if prefix else key)
    elif isinstance(node, list):
        for index, value in enumerate(node):
            yield from flatten(value, '%s[%d]' % (prefix, index))
    else:
        yield prefix, node


def show(value):
    if value is None:
        return 'unmeasured'
    if isinstance(value, dict):
        return '{%s}' % ', '.join('%s=%s' % (key, show(value[key]))
                                  for key in ('offset', 'size', 'before', 'after')
                                  if key in value)
    if isinstance(value, list):
        return '{%s}' % ', '.join('0x%02x' % item for item in value)
    if isinstance(value, int):
        return '0x%x' % value
    return str(value)


def load(path=DEFAULT_TABLE):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except OSError as error:
        die('cannot read %s: %s' % (path, error))
    except json.JSONDecodeError as error:
        die('%s: invalid JSON: %s' % (path, error))
    if data.get('schema') != 1:
        die('%s: unsupported schema %r' % (path, data.get('schema')))
    firmwares = data.get('firmwares') or die('%s: no firmwares' % path)
    return data, {entry['release']: entry for entry in firmwares}


def find(firmwares, name):
    """Accept a release ("9.00") or a firmware version ("0x09008031").

    A build-specific version such as 0x11008001 selects the release whose mask
    matches it, because a report carries the full version while the table is
    keyed by release.
    """
    if name in firmwares:
        return firmwares[name]
    wanted = name.lower()
    for entry in firmwares.values():
        if str(entry.get('version', '')).lower() == wanted:
            return entry
    if wanted.startswith('0x'):
        try:
            value = int(wanted, 16)
        except ValueError:
            value = None
        if value is not None:
            for entry in firmwares.values():
                mask = as_hex(entry.get('version_mask'), 'version_mask')
                version = as_hex(entry.get('version'), 'version')
                if mask is not None and version is not None and \
                        (value & mask) == (version & mask):
                    return entry
    die('unknown firmware %r (have: %s)' % (name, ', '.join(sorted(firmwares))))


def validate(data, firmwares):
    errors = []
    if data.get('platform') != 'ps4':
        errors.append('platform must be "ps4"')
    if len(firmwares) != len(data['firmwares']):
        errors.append('duplicate firmware release')
    for release, entry in firmwares.items():
        where = 'firmware %s' % release
        for section in SECTIONS:
            if not isinstance(entry.get(section), dict):
                errors.append('%s: missing %s section' % (where, section))
        version = entry.get('version')
        try:
            if not isinstance(version, str) or len(version) != 10 or \
                    int(version, 16) < 0:
                raise ValueError
        except ValueError:
            errors.append('%s: version must be 0x + 8 hex digits' % where)
        as_hex(entry.get('version_mask'), where + '.version_mask')
        for path, value in flatten(entry):
            if isinstance(value, str):
                as_hex(value, '%s %s' % (where, path))
        gates = (entry.get('kernel') or {}).get('gates') or []
        if len(gates) != 2:
            errors.append('%s: expected two kernel gates (null offsets when unmeasured)' % where)
        for index, gate in enumerate(gates):
            as_hex(gate.get('offset'), '%s gate %d offset' % (where, index))
            size = gate.get('size')
            before, after = gate.get('before'), gate.get('after')
            if gate.get('offset') is None and any(value is not None for value in (size, before, after)):
                errors.append('%s: gate %d has bytes without a measured offset' % (where, index))
            if size is None and before is None and after is None:
                continue
            if not isinstance(size, int) or not 1 <= size <= 5:
                errors.append('%s: gate %d size must be 1..5' % (where, index))
                continue
            for name, bytes_ in (('before', before), ('after', after)):
                if not isinstance(bytes_, list) or len(bytes_) != size:
                    errors.append('%s: gate %d %s must hold %d bytes'
                                  % (where, index, name, size))
                elif any(not isinstance(item, int) or not 0 <= item <= 255
                         for item in bytes_):
                    errors.append('%s: gate %d %s byte out of range'
                                  % (where, index, name))
        module = (entry.get('avcap') or {}).get('module')
        if not isinstance(module, str) or not module.endswith('.sprx'):
            errors.append('%s: avcap.module must be an .sprx basename' % where)
        exports = (entry.get('avcap') or {}).get('exports')
        if exports is not None:
            for label in ('init', 'term', 'open_video', 'read_video', 'close'):
                if label not in exports:
                    errors.append('%s: avcap.exports lacks %s' % (where, label))
        site = (entry.get('capture') or {}).get('latch_site')
        if isinstance(site, dict):
            as_hex(site.get('offset'), where + ' capture.latch_site.offset')
            size = site.get('size')
            if not isinstance(size, int) or not 1 <= size <= 16:
                errors.append('%s: latch_site size must be 1..16' % where)
            else:
                for name in ('before', 'after'):
                    bytes_ = site.get(name)
                    if not isinstance(bytes_, list) or len(bytes_) != size:
                        errors.append('%s: latch_site %s must hold %d bytes'
                                      % (where, name, size))
    for release, entry in firmwares.items():
        where = 'firmware %s' % release
        if entry.get('measured_on') is not None:
            as_hex(entry['measured_on'], where + ' measured_on')
        for label, library in (entry.get('libraries') or {}).items():
            if not isinstance(library, dict):
                errors.append('%s: library %s must be an object' % (where, label))
                continue
            module = library.get('module')
            if not isinstance(module, str) or not module.endswith('.sprx'):
                errors.append('%s: library %s module must be an .sprx basename'
                              % (where, label))
            as_hex(library.get('rva'), '%s library %s.rva' % (where, label))
    if errors:
        for error in errors:
            print('error: %s' % error, file=sys.stderr)
        die('%d table error(s)' % len(errors))


def fields(entry, sections=SECTIONS):
    return dict(flatten({key: entry.get(key) for key in sections}))


def list_table(data, firmwares):
    for release in firmwares:
        entry = firmwares[release]
        print('%s  version=%s  mask=%s' % (
            release, entry['version'], entry.get('version_mask', '?')))
        values = fields(entry)
        width = max(len(path) for path in values)
        for path in sorted(values):
            print('  %-*s  %s' % (width, path, show(values[path])))
        for evidence in entry.get('evidence') or []:
            print('  source: %s' % evidence)
        print()


def numeric(value):
    """Return an int for numbers and 0x strings, else None (bytes, prose)."""
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.lower().startswith('0x'):
        try:
            return int(value, 16)
        except ValueError:
            return None
    return None


def delta_text(va, vb):
    a, b = numeric(va), numeric(vb)
    if a is None or b is None:
        if va is None or vb is None:
            return 'unmeasured'
        return 'same' if va == vb else 'changed'
    if a == b:
        return 'same'
    return '%+#x' % (b - a)


def compare(firmwares, left, right):
    a = fields(find(firmwares, left))
    b = fields(find(firmwares, right))
    rows = []
    for path in sorted(set(a) | set(b)):
        va, vb = a.get(path), b.get(path)
        if va is None and vb is None:
            continue
        rows.append((path, show(va), show(vb), delta_text(va, vb)))
    width = max(len(row[0]) for row in rows)
    print('%-*s  %-14s  %-14s  %s' % (width, 'field', left, right, 'delta'))
    for path, va, vb, verdict in rows:
        print('%-*s  %-14s  %-14s  %s' % (width, path, va, vb, verdict))
    same = sum(1 for row in rows if row[3] == 'same')
    changed = sum(1 for row in rows if row[3] not in ('same', 'unmeasured'))
    unmeasured = sum(1 for row in rows if row[3] == 'unmeasured')
    print('\n%d fields, %d identical, %d different, %d unmeasured on one side'
          % (len(rows), same, changed, unmeasured))


def markdown(firmwares):
    for release in firmwares:
        entry = firmwares[release]
        print('### %s (`%s`)\n' % (release, entry['version']))
        print('| Field | Value |\n| --- | --- |')
        for path, value in sorted(fields(entry).items()):
            print('| `%s` | `%s` |' % (path, show(value)))
        print()
        if entry.get('notes'):
            print('%s\n' % entry['notes'])
        if entry.get('evidence'):
            print('Evidence: %s\n' % '; '.join(entry['evidence']))
    releases = sorted(firmwares)
    if len(releases) > 1:
        print('## Differences\n')
        for index, left in enumerate(releases):
            for right in releases[index + 1:]:
                print('### %s vs %s\n' % (left, right))
                print('| Field | %s | %s | Delta |' % (left, right))
                print('| --- | --- | --- | --- |')
                a, b = fields(firmwares[left]), fields(firmwares[right])
                for path in sorted(set(a) | set(b)):
                    va, vb = a.get(path), b.get(path)
                    if va is None and vb is None:
                        continue
                    delta = delta_text(va, vb)
                    print('| `%s` | `%s` | `%s` | %s |'
                          % (path, show(va), show(vb),
                             '`%s`' % delta if delta not in ('same', 'unmeasured',
                                                             'changed') else delta))
                print()


def initializer(firmwares, release):
    entry = find(firmwares, release)
    release = entry.get('release', release)  # accept a version as selector
    kernel = entry['kernel']
    avcap = entry['avcap']
    print('  .name = "PS4 %s / dumped on console",' % release)
    print('  .version = %su, .version_mask = %su,' % (
        entry['version'], entry.get('version_mask', '0xffff0000')))
    print('  .acmgr_offset = %s,' % (kernel.get('acmgr_offset') or
                                     '/* TODO acmgr_offset */ 0'))
    signature = kernel.get('acmgr_signature')
    if signature:
        print('  .acmgr_signature = {%s},'
              % ', '.join('0x%02x' % byte for byte in signature))
    else:
        print('  /* TODO acmgr_signature: needs a discovered kernel layout */')
    gates = kernel.get('gates') or []
    print('  .gates = {')
    for index, gate in enumerate(gates):
        offset = gate.get('offset')
        size = gate.get('size')
        before, after = gate.get('before'), gate.get('after')
        if None in (offset, size, before, after):
            print('    /* TODO gate %d: %s */'
                  % (index, offset or 'offset unmeasured'))
            continue
        print('    {%s, {%s}, {%s}, %d},' % (
            offset, ', '.join('0x%02x' % byte for byte in before),
            ', '.join('0x%02x' % byte for byte in after), size))
    print('  },')
    for field, key in (('avcap_init_offset', 'init'),
                       ('avcap_trap_offset', 'trap'),
                       ('ready_delta', 'ready_delta'),
                       ('dequeue_delta', 'dequeue_delta'),
                       ('consume_delta', 'consume_delta'),
                       ('ring_state_offset', 'ring_state_offset'),
                       ('rp_nn_socket', 'nn_socket'),
                       ('rp_vtable', 'vtable'),
                       ('rp_data', 'data'),
                       ('rp_data_size', 'data_size'),
                       ('rp_ensure_open', 'ensure_open'),
                       ('capture_flags', 'flags'),
                       ('capture_slots', 'slots'),
                       ('capture_object_head', 'object_head'),
                       ('capture_profile2_vtable', 'profile2_vtable'),
                       ('capture_check', 'check'),
                       ('capture_caps', 'caps')):
        section = 'remoteplay' if field.startswith('rp_') else \
                  'capture' if field.startswith('capture_') else 'avcap'
        value = (entry[section] or {}).get(key)
        if value is None:
            print('  /* TODO %s: unmeasured on %s */' % (field, release))
        else:
            print('  .%s = %s,' % (field, value))


def report_check(data, firmwares, report_path, release):
    entry = find(firmwares, release)
    try:
        text = Path(report_path).read_text(encoding='utf-8', errors='replace')
    except OSError as error:
        die('cannot read %s: %s' % (report_path, error))
    firmware, kernel, _offsets, symbols = parse_report(text)
    if firmware is None:
        die('%s: no PS4-DUMP header' % report_path)
    wanted = int(entry['version'], 16)
    failures = 0
    rows = []

    def check(label, expected, actual):
        nonlocal failures
        if expected is None:
            rows.append(('TODO', label, 'unmeasured', show(actual)))
        elif actual is None:
            rows.append(('TODO', label, show(expected), 'not reported'))
        elif expected == actual:
            rows.append(('OK', label, show(expected), show(actual)))
        else:
            failures += 1
            rows.append(('FAIL', label, show(expected), show(actual)))

    mask = as_hex(entry.get('version_mask'), release + '.version_mask')
    if (firmware & mask) != (wanted & mask):
        failures += 1
        rows.append(('FAIL', 'firmware', show(wanted), '0x%08x' % firmware))
    else:
        rows.append(('OK', 'firmware', '0x%08x' % wanted, '0x%08x' % firmware))
    exports = (entry.get('avcap') or {}).get('exports') or {}
    libraries = entry.get('libraries') or {}
    seen = set()
    for symbol in symbols:
        if symbol['rva'] is None:
            continue
        label = symbol['label']
        if label in seen:
            continue
        seen.add(label)
        key = label[len('avcap_'):] if label.startswith('avcap_') else label
        expected = None
        if label in libraries:
            library = libraries[label]
            module = library.get('module')
            if module and symbol['module'] != module:
                failures += 1
                rows.append(('FAIL', 'module.' + label, module, symbol['module']))
            expected = as_hex(library.get('rva'), 'library ' + label)
        else:
            path = LABEL_PATHS.get(label)
            if path:
                section, _, field = path.partition('.')
                expected = as_hex((entry.get(section) or {}).get(field), path)
            elif key in exports:
                expected = as_hex(exports[key], 'avcap.exports.' + key)
        check('symbol.' + label, expected, symbol['rva'])
    if kernel and kernel.get('status') == 'discovered':
        check('kernel.acmgr_offset',
              as_hex(entry['kernel'].get('acmgr_offset'), 'acmgr'),
              kernel.get('acmgr'))
        for index in (0, 1):
            gate = (entry['kernel'].get('gates') or [{}] * 2)[index] \
                if index < len(entry['kernel'].get('gates') or []) else {}
            check('kernel.gates[%d].offset' % index,
                  as_hex(gate.get('offset'), 'gate%d' % index),
                  kernel.get('gate%d' % index))
    width = max(len(row[1]) for row in rows)
    for status, label, expected, actual in rows:
        print('%-4s %-*s table=%-12s report=%s'
              % (status, width, label, expected, actual))
    print('\n%s: %d failure(s)' % (report_path, failures))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--table', default=str(DEFAULT_TABLE))
    parser.add_argument('--list', action='store_true', help='print every firmware')
    parser.add_argument('--markdown', action='store_true',
                        help='print the documentation tables')
    parser.add_argument('--compare', nargs=2, metavar=('LEFT', 'RIGHT'),
                        help='compare two releases')
    parser.add_argument('--c', metavar='RELEASE',
                        help='print a pbs_ps4_firmware initializer')
    parser.add_argument('--report', metavar='PATH',
                        help='cross-check a PS4-DUMP report')
    parser.add_argument('--firmware', metavar='RELEASE',
                        help='release to check the report against')
    parser.add_argument('--check', action='store_true',
                        help='validate the table and exit')
    args = parser.parse_args()
    data, firmwares = load(args.table)
    validate(data, firmwares)
    if args.check:
        print('%s: %d firmware(s) valid' % (args.table, len(firmwares)))
        return 0
    if args.report:
        if not args.firmware:
            die('--report requires --firmware RELEASE')
        return 1 if report_check(data, firmwares, args.report, args.firmware) else 0
    if args.compare:
        compare(firmwares, *args.compare)
        return 0
    if args.c:
        initializer(firmwares, args.c)
        return 0
    if args.markdown:
        markdown(firmwares)
        return 0
    list_table(data, firmwares)
    return 0


if __name__ == '__main__':
    sys.exit(main())
