#!/usr/bin/env python3
"""Cross-check and render the PS5 firmware/ABI ledger.

The values themselves live in ``payload/src/platform/ps5/offset/<name>.c``: that
is the only source of truth. ``tools/data/ps5_firmware_offsets.json`` records the
status, the evidence and the releases that are refused on purpose, and
``--check`` proves that the two agree with each other, with ``registry.c``, with
``FIRMWARE_RANGES`` in the generator and with the public release catalog.

The selector is modelled here as ``select()``. It must answer exactly like the
compiled C switch for every catalogued release, otherwise the ledger is stale.

Examples::

    python tools/ps5_firmware_table.py --list
    python tools/ps5_firmware_table.py --markdown
    python tools/ps5_firmware_table.py --compare 13.00 13.42
    python tools/ps5_firmware_table.py --unknown
    python tools/ps5_firmware_table.py --check
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TABLE = ROOT / "tools" / "data" / "ps5_firmware_offsets.json"
CATALOG = ROOT / "tools" / "ps5_firmware_releases.json"
GENERATOR = ROOT / "tools" / "generate_ps5_offsets.py"
DEFAULT_PROFILES = ROOT / "payload" / "src" / "platform" / "ps5" / "offset"

STATUSES = ("exact", "inferred", "unmeasured")
EXPECTED_KEYS = ("request_size", "kind_offset", "native_size_store_rva")
EXPERIMENTAL_DEFINE = "PBS_PS5_EXPERIMENTAL_FW"
EXPERIMENTAL_DEFINE_TEMPLATE = EXPERIMENTAL_DEFINE + "%s"
DOC_BEGIN = "<!-- ps5-offsets:begin -->"
DOC_END = "<!-- ps5-offsets:end -->"

FIELDS = (
    "avcap2.open_video_rva",
    "avcap2.ipc_command_instruction_rva",
    "avcap2.request_size",
    "avcap2.kind_offset",
    "remoteplay.request_size_store_rva",
    "remoteplay.rate.offset",
    "remoteplay.rate.size",
    "remoteplay.rate.descriptor_size",
    "remoteplay.image.offset",
    "remoteplay.image.size",
    "remoteplay.image.descriptor_size",
    "remoteplay.width_offset",
    "remoteplay.height_offset",
    "remoteplay.paste_offset",
    "remoteplay.video_mode_offset",
    "remoteplay.mask_mode_offset",
    "remoteplay.mask_span",
)


def die(message):
    sys.stderr.write("error: %s\n" % message)
    raise SystemExit(2)


def as_hex(value):
    return "0x%x" % value


def firmware_id(value):
    return "0x%08x" % value


def as_int(text, where):
    if isinstance(text, int):
        return text
    if not isinstance(text, str) or not re.fullmatch(r"0x[0-9a-fA-F]+", text):
        raise ValueError("%s: expected a 0x-prefixed integer, got %r" % (where, text))
    return int(text, 16)


def release_version(release):
    """13.42 -> 0x13420000, matching catalog_release_keys() in the generator."""
    parts = release.split(".")
    if len(parts) < 2 or not parts[0] or len(parts[1]) != 2:
        raise ValueError("release %r is not MM.mm[.pp]" % release)
    return int(parts[0] + parts[1], 16) << 16


def _value(text, pattern, where, default=None):
    match = re.search(pattern, text)
    if not match:
        if default is not None:
            return default
        raise ValueError("%s: missing %s" % (where, pattern))
    return int(match.group(1), 16)


def _block(text, name, where):
    match = re.search(r"\.%s\s*=\s*\{(.*?)\}," % name, text, re.S)
    if not match:
        raise ValueError("%s: missing .%s group" % (where, name))
    return match.group(1)


def parse_profile(path):
    text = path.read_text(encoding="utf-8")
    where = path.name
    rate = _block(text, "rate", where)
    image = _block(text, "image", where)
    profile = {
        "name": path.stem,
        "firmware": _value(text, r"\.firmware\s*=\s*0x([0-9a-fA-F]+)u", where),
        "experimental": "EXPERIMENTAL" in text,
        "avcap2": {
            "open_video_rva": _value(text, r"\.open_video_rva\s*=\s*0x([0-9a-fA-F]+)u", where),
            "ipc_command_instruction_rva": _value(
                text, r"\.ipc_command_instruction_rva\s*=\s*0x([0-9a-fA-F]+)u", where),
            "request_size": _value(text, r"\.request_size\s*=\s*0x([0-9a-fA-F]+)u", where),
            "kind_offset": _value(text, r"\.kind_offset\s*=\s*0x([0-9a-fA-F]+)u", where),
        },
        "remoteplay": {
            "request_size_store_rva": _value(
                text, r"\.request_size_store_rva\s*=\s*0x([0-9a-fA-F]+)u", where),
            "rate": {
                "offset": _value(rate, r"\.offset\s*=\s*0x([0-9a-fA-F]+)u", where),
                "size": _value(rate, r"\.size\s*=\s*0x([0-9a-fA-F]+)u", where),
                "descriptor_size": _value(
                    rate, r"\.descriptor_size\s*=\s*0x([0-9a-fA-F]+)u", where),
            },
            "image": {
                "offset": _value(image, r"\.offset\s*=\s*0x([0-9a-fA-F]+)u", where),
                "size": _value(image, r"\.size\s*=\s*0x([0-9a-fA-F]+)u", where),
                "descriptor_size": _value(
                    image, r"\.descriptor_size\s*=\s*0x([0-9a-fA-F]+)u", where),
            },
            "width_offset": _value(text, r"\.width_offset\s*=\s*0x([0-9a-fA-F]+)u", where),
            "height_offset": _value(text, r"\.height_offset\s*=\s*0x([0-9a-fA-F]+)u", where),
            "paste_offset": _value(text, r"\.paste_offset\s*=\s*0x([0-9a-fA-F]+)u", where),
            "video_mode_offset": _value(
                text, r"\.video_mode_offset\s*=\s*0x([0-9a-fA-F]+)u", where),
            "mask_mode_offset": _value(
                text, r"\.mask_mode_offset\s*=\s*0x([0-9a-fA-F]+)u", where),
            "mask_span": _value(text, r"\.mask_span\s*=\s*0x([0-9a-fA-F]+)u", where, default=0x24),
        },
    }
    return profile


def load_profiles(directory=DEFAULT_PROFILES):
    profiles = {}
    for path in sorted(Path(directory).glob("*.c")):
        if path.name == "registry.c":
            continue
        profile = parse_profile(path)
        if profile["name"] in profiles:
            raise ValueError("duplicate profile file %s" % path.name)
        profiles[profile["name"]] = profile
    if not profiles:
        raise ValueError("no firmware profile in %s" % directory)
    return profiles


def parse_registry(path=DEFAULT_PROFILES / "registry.c"):
    text = Path(path).read_text(encoding="utf-8")
    marker = "const uint32_t version = firmware"
    if marker not in text:
        raise ValueError("%s: family fallback not found" % path)
    head, tail = text.split(marker, 1)
    exact = {int(firmware, 16): name for firmware, name in
             re.findall(r"case 0x([0-9a-fA-F]{8})u: return &pbs_ps5_offsets_([0-9a-f]+);", head)}
    whitelist = {int(value, 16) for value in re.findall(r"case 0x([0-9a-fA-F]{8})u:", tail)}
    ranges = [(int(low, 16), int(high, 16), name) for low, high, name in re.findall(
        r"if \(version >= 0x([0-9a-fA-F]{8})u && version <= 0x([0-9a-fA-F]{8})u\)\s*"
        r"\n\s*return &pbs_ps5_offsets_([0-9a-f]+);", tail)]
    experimental = set()
    gates = {}
    for match in re.finditer(r"#ifdef (PBS_PS5_EXPERIMENTAL_FW\w+)(.*?)#endif", text, re.S):
        found = {int(value, 16) for value in re.findall(r"case 0x([0-9a-fA-F]{8})u:", match.group(2))}
        gates[match.group(1)] = found
        experimental |= found
    return {"exact": exact, "whitelist": whitelist, "ranges": ranges,
            "experimental": experimental, "gates": gates}


def parse_generator_ranges(path=GENERATOR):
    text = Path(path).read_text(encoding="utf-8")
    start = text.index("FIRMWARE_RANGES = (")
    block = text[start:text.index("\n)\n", start)]
    return [(int(low, 16), int(high, 16), int(anchor, 16)) for low, high, anchor in
            re.findall(r"\((0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\)", block)]


def catalog_releases(path=CATALOG):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    releases = data["releases"]
    if len(releases) != len(set(releases)):
        raise ValueError("%s: duplicate release" % path)
    return releases


def select(firmware, registry, gated=False):
    """Mirror pbs_ps5_find_offsets(): exact id first, then an audited family.

    Builds without a -DPBS_PS5_EXPERIMENTAL_FW<name> define do not compile the
    gated cases at all, so `gated=False` models the stock payload and
    `gated=True` models an opt-in build for one specific profile.
    """
    if firmware in registry["exact"]:
        if firmware in registry["experimental"] and not gated:
            return None, "gated"
        return registry["exact"][firmware], "exact"
    version = firmware & 0xFFFF0000
    if version not in registry["whitelist"]:
        return None, "not-catalogued"
    for low, high, name in registry["ranges"]:
        if low <= version <= high:
            return name, "family-range"
    return None, "gap"


def field(profile, path):
    node = profile
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def load_table(path=DEFAULT_TABLE):
    table = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(table, dict) or table.get("schema") != 1:
        raise ValueError("%s: unsupported schema" % path)
    return table


def validate(table, profiles, path=DEFAULT_TABLE):
    errors = []
    for key in ("firmwares", "ranges"):
        if not isinstance(table.get(key), list) or not table[key]:
            errors.append("%s: missing list %r" % (path, key))
    if errors:
        return errors

    seen_firmware, seen_profile, seen_release = {}, {}, {}
    for index, row in enumerate(table["firmwares"]):
        where = "firmwares[%d]" % index
        if not isinstance(row, dict):
            errors.append("%s: not an object" % where)
            continue
        release = row.get("release")
        if not isinstance(release, str) or not re.fullmatch(r"\d{1,2}\.\d{2}", release):
            errors.append("%s: bad release %r" % (where, release))
            continue
        where = "firmware %s" % release
        try:
            firmware = as_int(row.get("firmware"), where)
        except ValueError as error:
            errors.append(str(error))
            continue
        # The low 16 bits are build revisions: only the release anchor is fixed.
        if firmware & 0xFFFF0000 != release_version(release):
            errors.append("%s: release maps to 0x%08x, row says 0x%08x"
                          % (where, release_version(release), firmware))
        if firmware in seen_firmware:
            errors.append("%s: firmware already listed as %s" % (where, seen_firmware[firmware]))
        seen_firmware[firmware] = release
        if release in seen_release:
            errors.append("%s: release listed twice" % where)
        seen_release[release] = True

        status = row.get("status")
        if status not in STATUSES:
            errors.append("%s: unknown status %r" % (where, status))
            continue
        if status in ("exact", "inferred"):
            name = row.get("profile")
            if not isinstance(name, str) or name not in profiles:
                errors.append("%s: unknown profile %r" % (where, name))
                continue
            if name in seen_profile:
                errors.append("%s: profile %s already used by %s"
                              % (where, name, seen_profile[name]))
            seen_profile[name] = release
            if profiles[name]["firmware"] != firmware:
                errors.append("%s: profile %s declares 0x%08x"
                              % (where, name, profiles[name]["firmware"]))
            expected = row.get("expected", {})
            if not isinstance(expected, dict) or not expected:
                errors.append("%s: %s rows need an expected block" % (where, status))
                continue
            for key in expected:
                if key not in EXPECTED_KEYS:
                    errors.append("%s: unexpected expected key %r" % (where, key))
            remote = profiles[name]["remoteplay"]
            actual = {"request_size": profiles[name]["avcap2"]["request_size"],
                      "kind_offset": profiles[name]["avcap2"]["kind_offset"],
                      "native_size_store_rva": remote["request_size_store_rva"]}
            for key, value in expected.items():
                if key not in actual:
                    continue
                if as_int(value, "%s.expected.%s" % (where, key)) != actual[key]:
                    errors.append("%s: expected %s %s, profile %s has %s"
                                  % (where, key, value, name, as_hex(actual[key])))
            if bool(row.get("experimental")) != profiles[name]["experimental"]:
                errors.append("%s: experimental flag disagrees with %s.c" % (where, name))
            if status == "inferred":
                if not row.get("experimental"):
                    errors.append("%s: inferred rows must be gated experiments" % where)
                source = row.get("inferred_from")
                if source not in profiles:
                    errors.append("%s: unknown inferred_from %r" % (where, source))
                elif source != name:
                    drifted = [path for path in FIELDS
                               if field(profiles[name], path) != field(profiles[source], path)]
                    if drifted:
                        errors.append("%s: %s.c drifted from its inferred source %s.c (%s)"
                                      % (where, name, source, ", ".join(drifted)))
        else:
            if row.get("profile"):
                errors.append("%s: unmeasured rows must not name a profile" % where)
            if not str(row.get("reason", "")).strip():
                errors.append("%s: unmeasured rows need a reason" % where)
            candidates = row.get("candidates") or []
            if not isinstance(candidates, list) or not candidates:
                errors.append("%s: unmeasured rows need at least one candidate" % where)
            for name in candidates:
                if name not in profiles:
                    errors.append("%s: unknown candidate profile %r" % (where, name))
            if len(candidates) >= 2 and len({
                    profiles[name]["avcap2"]["request_size"] for name in candidates
                    if name in profiles}) < 2:
                errors.append("%s: candidates share a request size, the disagreement "
                              "recorded here no longer exists" % where)

    for name in sorted(set(profiles) - set(seen_profile)):
        errors.append("profile %s has no exact ledger row" % name)

    for index, row in enumerate(table["ranges"]):
        where = "ranges[%d]" % index
        if not isinstance(row, dict):
            errors.append("%s: not an object" % where)
            continue
        try:
            low = as_int(row.get("from"), where)
            high = as_int(row.get("to"), where)
        except ValueError as error:
            errors.append(str(error))
            continue
        if low > high:
            errors.append("%s: from 0x%08x is above to 0x%08x" % (where, low, high))
        if low & 0xFFFF or high & 0xFFFF:
            errors.append("%s: range bounds must be release anchors" % where)
        name = row.get("profile")
        if name not in profiles:
            errors.append("%s: unknown profile %r" % (where, name))
        elif not low <= profiles[name]["firmware"] & 0xFFFF0000 <= high:
            errors.append("%s: profile %s is outside its own range" % (where, name))
    return errors


def check(table, profiles, registry, generator_ranges, releases):
    errors = list(validate(table, profiles))
    if errors:
        return errors, []

    # 1. The registry must return every ledger profile for its exact id, and a
    # gated profile must sit behind its own #ifdef instead of being selected.
    for row in table["firmwares"]:
        if row["status"] not in ("exact", "inferred"):
            continue
        firmware = as_int(row["firmware"], row["release"])
        gated = row["status"] == "inferred" or bool(row.get("experimental"))
        # An opt-in build with its own define must select the profile; a stock
        # build must refuse it.
        name, mode = select(firmware, registry, gated=gated)
        if mode != "exact" or name != row["profile"]:
            errors.append("%s: selector returns %s/%s instead of %s/exact"
                          % (row["release"], name, mode, row["profile"]))
        if gated:
            name, mode = select(firmware, registry)
            if mode != "gated":
                errors.append("%s: stock build must refuse a gated profile, selector says %s"
                              % (row["release"], mode))
        define = EXPERIMENTAL_DEFINE_TEMPLATE % row["profile"].upper()
        if row.get("experimental"):
            if firmware not in registry["gates"].get(define, set()):
                errors.append("%s: profile is not gated by #ifdef %s" % (row["release"], define))
        elif firmware in registry["experimental"]:
            errors.append("%s: measured profile is gated, it should be selectable"
                          % row["release"])

    # 2. Ranges: ledger, generator constants and compiled switch must agree.
    def anchor_profile(anchor):
        for name, profile in profiles.items():
            if profile["firmware"] == anchor:
                return name
        return None

    expected_ranges = []
    for low, high, anchor in generator_ranges:
        name = anchor_profile(anchor)
        if name is None:
            errors.append("generator range 0x%08x-0x%08x: no profile with firmware 0x%08x"
                          % (low, high, anchor))
            continue
        expected_ranges.append((low, high, name))
    ledger_ranges = [(as_int(row["from"], row["profile"]), as_int(row["to"], row["profile"]),
                      row["profile"]) for row in table["ranges"]]
    if sorted(ledger_ranges) != sorted(expected_ranges):
        errors.append("ledger ranges %s differ from FIRMWARE_RANGES %s"
                      % (ledger_ranges, expected_ranges))
    compiled = [(low, high, "pbs_ps5_offsets_" + name) for low, high, name in registry["ranges"]]
    if [(low, high, "pbs_ps5_offsets_" + name) for low, high, name in ledger_ranges] != compiled:
        errors.append("registry.c ranges %s differ from the ledger %s" % (compiled, ledger_ranges))

    # 3. The whitelist must be exactly the catalogued releases inside a range.
    inside = {release_version(release) for release in releases
              if any(low <= release_version(release) <= high for low, high, _ in expected_ranges)}
    if registry["whitelist"] != inside:
        missing = sorted(as_hex(value) for value in inside - registry["whitelist"])
        extra = sorted(as_hex(value) for value in registry["whitelist"] - inside)
        errors.append("registry.c whitelist disagrees with the catalog: missing %s extra %s"
                      % (missing, extra))

    # 4. Every catalogued release must agree with the selector. Releases the
    # selector refuses are allowed to have no row (legacy firmware, no support
    # attempt); they are reported instead of being silently ignored.
    by_firmware = {}
    for row in table["firmwares"]:
        by_firmware[as_int(row["firmware"], row["release"])] = row
    notes = []
    unlisted = []
    for release in releases:
        version = release_version(release)
        name, mode = select(version, registry)
        row = by_firmware.get(version)
        if mode == "exact":
            if not row or row["status"] != "exact" or row["profile"] != name:
                errors.append("%s: selector returns exact profile %s but the ledger does not say so"
                              % (release, name))
        elif mode == "gated":
            if not row or row["status"] != "inferred":
                errors.append("%s: selector refuses a gated profile, so the ledger must mark "
                              "it inferred" % release)
        elif mode == "family-range":
            if row and row["status"] == "exact":
                errors.append("%s: ledger claims an exact profile, the selector reuses %s"
                              % (release, name))
            elif row and row["status"] == "inferred":
                errors.append("%s: ledger claims an inferred profile, the selector reuses %s"
                              % (release, name))
            elif row and row["status"] == "unmeasured":
                errors.append("%s: ledger says unmeasured, the selector reuses %s"
                              % (release, name))
        elif not row:
            unlisted.append(version)
        elif row["status"] in ("exact", "inferred"):
            errors.append("%s: ledger claims a profile, the selector refuses it (%s)"
                          % (release, mode))
    if unlisted:
        unique = sorted(set(unlisted))
        exact_builds = {}
        for row in table["firmwares"]:
            if row["status"] == "exact":
                firmware = as_int(row["firmware"], row["release"])
                exact_builds.setdefault(firmware & 0xFFFF0000, []).append(firmware_id(firmware))
        parts = []
        for version in unique:
            builds = exact_builds.get(version)
            parts.append(_release_text(version)
                         + (" (only %s)" % ", ".join(sorted(builds)) if builds else ""))
        notes.append("%d catalogued release anchor(s) refused without a ledger row: %s"
                     % (len(unique), " ".join(parts)))

    # 5. Unmeasured rows must really be refused.
    for row in table["firmwares"]:
        if row["status"] != "unmeasured":
            continue
        firmware = as_int(row["firmware"], row["release"])
        name, mode = select(firmware, registry)
        if name is not None:
            errors.append("%s: listed as unmeasured but the selector returns %s/%s"
                          % (row["release"], name, mode))
    return errors, notes


def exact_rows(table):
    return [row for row in table["firmwares"] if row["status"] == "exact"]


def unmeasured_rows(table):
    return [row for row in table["firmwares"] if row["status"] == "unmeasured"]


def other_builds(row, registry):
    """What happens to another build revision of the same release."""
    version = as_int(row["firmware"], row["release"]) & 0xFFFF0000
    if version in registry["whitelist"]:
        name, mode = select(version | 1, registry)
        return "%s (%s)" % (name, mode) if name else "refused (%s)" % mode
    return "refused (exact-only)"


def list_rows(table, profiles, registry):
    print("%-6s %-11s %-10s %-8s %-9s %-9s %s"
          % ("release", "firmware", "status", "profile", "request", "kind", "other builds"))
    for row in sorted(table["firmwares"], key=lambda item: as_int(item["firmware"], item["release"])):
        status = row["status"]
        if status == "unmeasured":
            print("%-6s %-11s %-10s %-8s %-9s %-9s %s"
                  % (row["release"], row["firmware"], status, "-", "-", "-",
                     "refused: " + ", ".join(row.get("candidates", []))))
            continue
        profile = profiles[row["profile"]]
        note = other_builds(row, registry)
        if status == "inferred":
            note = "gated, inferred from %s; other builds: %s" % (
                row.get("inferred_from", "?"), note)
        print("%-6s %-11s %-10s %-8s %-9s %-9s %s"
              % (row["release"], row["firmware"], status, row["profile"],
                 as_hex(profile["avcap2"]["request_size"]),
                 as_hex(profile["avcap2"]["kind_offset"]), note))


def markdown(table, profiles, registry):
    print("| Firmware | FW identifier | Profile | Request size | Kind offset | Native size store RVA | Source |")
    print("|---|---|---|---|---|---|---|")
    for row in sorted(exact_rows(table), key=lambda item: as_int(item["firmware"], item["release"])):
        profile = profiles[row["profile"]]
        print("| %s | `%s` | `%s.c` | `%s` | `%s` | `%s` | `%s` |"
              % (row["release"], row["firmware"], row["profile"],
                 as_hex(profile["avcap2"]["request_size"]),
                 as_hex(profile["avcap2"]["kind_offset"]),
                 as_hex(profile["remoteplay"]["request_size_store_rva"]),
                 row.get("source", "not recorded")))
    print()
    inferred = [row for row in table["firmwares"] if row["status"] == "inferred"]
    if inferred:
        print("| Firmware | FW identifier | Profile | Request size | Kind offset | Inferred from | Enable with |")
        print("|---|---|---|---|---|---|---|")
        for row in sorted(inferred, key=lambda item: as_int(item["firmware"], item["release"])):
            profile = profiles[row["profile"]]
            print("| %s | `%s` | `%s.c` | `%s` | `%s` | `%s.c` | `-D%s%s` |"
                  % (row["release"], row["firmware"], row["profile"],
                     as_hex(profile["avcap2"]["request_size"]),
                     as_hex(profile["avcap2"]["kind_offset"]),
                     row.get("inferred_from", "?"),
                     EXPERIMENTAL_DEFINE, row["profile"].upper()))
        print()
    print("| Family range | Profile | Other builds | Evidence |")
    print("|---|---|---|---|")
    for row in table["ranges"]:
        print("| %s - %s | `%s.c` | reused | %s |"
              % (_release_text(as_int(row["from"], "range")),
                 _release_text(as_int(row["to"], "range")),
                 row["profile"], row.get("notes", "")))
    print()
    if unmeasured_rows(table):
        print("| Refused release | FW identifier | Why | Candidates | Missing |")
        print("|---|---|---|---|---|")
        for row in sorted(unmeasured_rows(table),
                          key=lambda item: as_int(item["firmware"], item["release"])):
            print("| %s | `%s` | %s | %s | %s |"
                  % (row["release"], row["firmware"], row.get("reason", ""),
                     ", ".join("`%s.c`" % name for name in row.get("candidates", [])),
                     ", ".join("`%s`" % name for name in row.get("missing", []))))


def _release_text(version):
    return "%x.%02x" % (version >> 24, (version >> 16) & 0xFF)


def resolve(table, profiles, selector):
    if selector in profiles:
        return selector
    for row in table["firmwares"]:
        if row["release"] == selector:
            if row["status"] == "unmeasured":
                die("%s has no profile (%s)" % (selector, row.get("reason", row["status"])))
            return row["profile"]
    die("unknown firmware or profile %r" % selector)


def compare(table, profiles, left, right):
    left_name, right_name = resolve(table, profiles, left), resolve(table, profiles, right)
    a, b = profiles[left_name], profiles[right_name]
    print("| Field | %s (`%s`) | %s (`%s`) | Delta |"
          % (left, firmware_id(a["firmware"]), right, firmware_id(b["firmware"])))
    print("|---|---|---|---|")
    for path in FIELDS:
        va, vb = field(a, path), field(b, path)
        if va is None or vb is None:
            continue
        delta = vb - va
        text = "same" if delta == 0 else ("+0x%x" % delta if delta > 0 else "-0x%x" % -delta)
        print("| `%s` | `%s` | `%s` | %s |"
              % (path, as_hex(va), as_hex(vb), text if delta == 0 else "`%s`" % text))
    print()
    if a["avcap2"]["request_size"] == b["avcap2"]["request_size"]:
        print("Request geometry identical; only code addresses differ.")
    else:
        print("Request geometry differs: these two releases cannot share a profile.")
    for row in table["firmwares"]:
        if row["status"] == "inferred" and row["profile"] in (left_name, right_name):
            print()
            print("`%s.c` is inferred from `%s.c` and is gated: it is not selected by "
                  "a stock build." % (row["profile"], row.get("inferred_from", "?")))


def unknown(table):
    rows = unmeasured_rows(table)
    if not rows:
        print("every catalogued release has a decision")
    for row in sorted(rows, key=lambda item: as_int(item["firmware"], item["release"])):
        print("%s (%s): %s" % (row["release"], to_display(row["firmware"]),
                               row.get("reason", "no reason recorded")))
        if row.get("candidates"):
            print("  candidates: %s" % ", ".join(row["candidates"]))
        if row.get("missing"):
            print("  missing: %s" % ", ".join(row["missing"]))
        if row.get("source"):
            print("  archive: %s%s" % (row["source"],
                                       " sha256 " + row["sha256"] if row.get("sha256") else ""))
        if row.get("evidence"):
            print("  evidence: %s" % row["evidence"])
    inferred = [row for row in table["firmwares"] if row["status"] == "inferred"]
    if inferred:
        print()
        print("inferred (compiled but gated; no hardware result yet):")
        for row in sorted(inferred, key=lambda item: as_int(item["firmware"], item["release"])):
            print("%s (%s): %s.c copies %s.c; enable with -D%s%s"
                  % (row["release"], to_display(row["firmware"]), row["profile"],
                     row.get("inferred_from", "?"), EXPERIMENTAL_DEFINE,
                     row["profile"].upper()))
            if row.get("notes"):
                print("  %s" % row["notes"])


def to_display(firmware):
    return firmware if isinstance(firmware, str) else firmware_id(firmware)


def write_doc(table, profiles, registry, path):
    document = Path(path)
    if not document.exists():
        die("%s does not exist" % document)
    text = document.read_text(encoding="utf-8")
    if DOC_BEGIN not in text or DOC_END not in text:
        die("%s needs the %s / %s markers" % (document, DOC_BEGIN, DOC_END))
    body = render_markdown(table, profiles, registry)
    head, rest = text.split(DOC_BEGIN, 1)
    _, tail = rest.split(DOC_END, 1)
    document.write_text(head + DOC_BEGIN + "\n" + body + DOC_END + tail, encoding="utf-8")
    print("%s: table written" % document)


def render_markdown(table, profiles, registry):
    import io
    from contextlib import redirect_stdout
    buffer = io.StringIO()
    with redirect_stdout(buffer):
        markdown(table, profiles, registry)
    return buffer.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--table", default=str(DEFAULT_TABLE))
    parser.add_argument("--profiles", default=str(DEFAULT_PROFILES))
    parser.add_argument("--list", action="store_true", help="print every firmware and its status")
    parser.add_argument("--markdown", action="store_true", help="print the documentation tables")
    parser.add_argument("--compare", nargs=2, metavar=("LEFT", "RIGHT"),
                        help="compare two releases or profiles")
    parser.add_argument("--unknown", action="store_true",
                        help="list the releases that are refused, with the missing evidence")
    parser.add_argument("--write", metavar="PATH",
                        help="replace the generated block in a document")
    parser.add_argument("--check", action="store_true",
                        help="cross-check ledger, profiles, registry, ranges and catalog")
    args = parser.parse_args()

    try:
        table = load_table(args.table)
        profiles = load_profiles(args.profiles)
        registry = parse_registry(Path(args.profiles) / "registry.c")
        generator_ranges = parse_generator_ranges()
        releases = catalog_releases()
    except (ValueError, OSError, KeyError) as error:
        die(str(error))

    if args.check:
        errors, notes = check(table, profiles, registry, generator_ranges, releases)
        for problem in errors:
            sys.stderr.write("FAIL %s\n" % problem)
        if errors:
            return 1
        for note in notes:
            print(note)
        print("%s: %d profiles, %d refused releases, %d ranges, %d catalogued releases valid"
              % (args.table, len(profiles), len(unmeasured_rows(table)),
                 len(table["ranges"]), len(releases)))
        return 0
    if args.unknown:
        unknown(table)
        return 0
    if args.compare:
        compare(table, profiles, *args.compare)
        return 0
    if args.write:
        write_doc(table, profiles, registry, args.write)
        return 0
    if args.markdown:
        markdown(table, profiles, registry)
        return 0
    list_rows(table, profiles, registry)
    return 0


if __name__ == "__main__":
    sys.exit(main())
