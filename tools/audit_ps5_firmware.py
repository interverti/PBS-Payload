#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read private dumps and report observed ABI families without widening support.

Requires the same Capstone/7-Zip dependencies as generate_ps5_offsets.py.
Outputs hashes, offsets and provenance only; never exports firmware bytes.
"""
from __future__ import annotations

import argparse
from datetime import date
import hashlib
import json
import struct
import subprocess
from pathlib import Path

import generate_ps5_offsets as gen


def sha(data):
    return hashlib.sha256(data).hexdigest()


def label(firmware):
    return f"{firmware >> 24:x}.{(firmware >> 16) & 255:02x}"


def geometry(layout):
    """Code addresses are not ABI fields. Equal geometry need not mean equal code."""
    return {k: v for k, v in sorted((layout | {"mask_span": layout.get("mask_span", 0x24)}).items())
            if k not in ("open_video", "ipc_command", "constructor")}


def elf_image(data):
    """Map bounded file-backed ELF64 LOADs; never interpret SELF as plaintext."""
    if data[:6] != b"\x7fELF\x02\x01" or len(data) < 64:
        raise ValueError("not plaintext little-endian ELF64")
    offset = struct.unpack_from("<Q", data, 32)[0]
    stride, count = struct.unpack_from("<HH", data, 54)
    if stride < 56 or count > 128 or offset + stride * count > len(data):
        raise ValueError("invalid program header table")
    memory, executable = bytearray(0x20000), []
    for i in range(count):
        kind, flags, pos, va, _, size, memsize, _ = struct.unpack_from("<IIQQQQQQ", data, offset + i * stride)
        if kind != 1:
            continue
        if pos + size > len(data) or size > memsize:
            raise ValueError("invalid LOAD extent")
        if flags & 1:
            executable.append({"rva": va, "size": size, "sha256": sha(data[pos:pos + size])})
        if va < len(memory):
            n = min(size, len(memory) - va)
            memory[va:va + n] = data[pos:pos + n]
    if not executable:
        raise ValueError("no executable LOAD")
    return bytes(memory), executable


def archive_file(archive, name, seven_zip):
    # Used only for the small explicitly selected plaintext Avcap2 files.
    listing = subprocess.run([seven_zip, "l", "-slt", str(archive)],
                             capture_output=True, text=True, check=True).stdout
    names = []
    import re
    for block in re.split(r"\n\s*\n", listing):
        fields = dict(line.split(" = ", 1) for line in block.splitlines() if " = " in line)
        member = fields.get("Path", "")
        if member.endswith(name):
            size = int(fields.get("Size", "0"))
            if not 0 < size <= 4 * 1024 * 1024:
                raise ValueError("selected ELF exceeds budget")
            names.append((member, size))
    if len(names) > 8 or len({n for n, _ in names}) != len(names):
        raise ValueError("ambiguous selected ELF members")
    result = []
    for member, size in names:
        data = subprocess.run([seven_zip, "x", "-so", str(archive), member],
                              capture_output=True, check=True).stdout
        if len(data) != size:
            raise ValueError("selected ELF length differs from archive metadata")
        result.append((member, data))
    return result


def collect(dumps, local_roots, reference, seven_zip):
    gathered, inventory = {}, []

    def add(name, data, source):
        match = gen.FW_RE.search(name)
        if match:
            fw = int(match.group(1), 16)
            key = "avcap" if name.endswith(gen.MODULE) else "eboot"
            gathered.setdefault(fw, {}).setdefault(key, []).append((data, source))

    for archive in sorted(dumps.iterdir()):
        if archive.suffix.lower() not in (".zip", ".rar", ".7z", ".tar"):
            continue
        data = gen.load_members(archive, seven_zip)
        inventory.append({"name": archive.name, "bytes": archive.stat().st_size,
                          "sha256": sha(archive.read_bytes()), "memory_members": len(data)})
        for name, image in data.items():
            add(name, image, f"{archive.name}:{name}")
    for root in local_roots:
        for suffix in (gen.MODULE, gen.EBOOT):
            for path in sorted(root.glob(f"ps5-*/{suffix}")):
                if not 0 < path.stat().st_size <= gen.memory_limit(path.name):
                    raise ValueError(f"local memory extent exceeds budget: {path}")
                add(str(path), path.read_bytes(), str(path))
    if reference:
        gathered.setdefault(0x09400008, {}).setdefault("avcap", []).append((reference.read_bytes(), str(reference)))
    return gathered, inventory


def analyze(gathered):
    results = []
    for fw, images in sorted(gathered.items()):
        entry = {"firmware": f"0x{fw:08x}", "version": label(fw),
                 "avcap": [], "constructors": [], "rejected": [], "hardware_tested_by_this_audit": False}
        for data, source in images.get("avcap", []):
            try:
                layout = gen.avcap_layout(data)
                entry["avcap"].append({"source": source, "bytes": len(data), "sha256": sha(data),
                    "layout": layout, "open_window_sha256": sha(data[layout["open_video"]:layout["open_video"] + 0x120])})
            except ValueError as error:
                entry["rejected"].append({"source": source, "reason": str(error)})
        layouts = {json.dumps(v["layout"], sort_keys=True) for v in entry["avcap"]}
        if len(layouts) != 1:
            entry["status"] = "missing-or-conflicting-avcap"
            results.append(entry)
            continue
        abi = entry["avcap"][0]["layout"]
        for data, source in images.get("eboot", []):
            try:
                native = gen.constructor_layout(data, abi["size"])
                entry["constructors"].append({"source": source, "bytes": len(data),
                    "sha256": sha(data), "layout": native})
            except ValueError as error:
                entry["rejected"].append({"source": source, "reason": str(error)})
        natives = {json.dumps(v["layout"], sort_keys=True) for v in entry["constructors"]}
        if len(natives) == 1:
            layout = abi | entry["constructors"][0]["layout"]
            entry["geometry"] = geometry(layout)
            entry["geometry_sha256"] = sha(json.dumps(entry["geometry"], sort_keys=True).encode())
            entry["status"] = "observed-avcap-and-native-constructor"
        else:
            entry["status"] = "missing-or-conflicting-native-constructor"
        results.append(entry)
    return results


def families(results):
    groups = []
    # Consecutive OBSERVED samples only: these are not lists of every release
    # in the interval and must never be used to enable missing versions.
    for record in results:
        if "geometry" not in record:
            continue
        signature = record["geometry_sha256"]
        if not groups or groups[-1]["geometry_sha256"] != signature:
            groups.append({"geometry_sha256": signature, "geometry": record["geometry"],
                           "observed_versions": [], "intermediate_releases_verified": False})
        groups[-1]["observed_versions"].append(record["version"])
    for group in groups:
        group["observed_span"] = [group["observed_versions"][0], group["observed_versions"][-1]]
    return groups


def render(report):
    complete = [r for r in report["firmware"] if r["status"] == "observed-avcap-and-native-constructor"]
    modern = [r for r in complete if int(r["firmware"], 16) >= 0x09000000]
    observed_span = f"{modern[0]['version']}–{modern[-1]['version']}" if modern else "nessuno"
    lines = ["# PS5: famiglie ABI osservate nei dump", "", f"Audit locale: {report['audit_date']}.", "",
        "Confronto statico di Avcap2 e dei costruttori RemotePlay. Nessun firmware viene eseguito, "
        "nessun nuovo intervallo viene abilitato nel payload. Un intervallo osservato unisce i campioni "
        "con la stessa geometria, non certifica le release intermedie.", "",
        "## Risultato utilizzabile", "",
        f"**Compatibilità ABI analizzata sui campioni PS5 {observed_span}; funzionamento "
        "delle singole versioni da verificare su console.** Gli estremi non certificano "
        "tutte le release comprese nell'intervallo. I cambi di ABI non sono localizzati "
        "su tutte le release intermedie.", "",
        "| Famiglia | Campioni con costruttore analizzato | Request | Kind | Rate offset/size | Image offset/size | Width/height | Video/mask |",
        "|---|---|---:|---:|---|---|---|---|"]
    for i, group in enumerate(report["families"], 1):
        g = group["geometry"]
        lines.append(f"| ABI-{i} | {', '.join(group['observed_versions'])} | `0x{g['size']:x}` | `0x{g['kind']:x}` | "
            f"`{g['rate_group']:#x}/{g['rate_group_size']:#x}` | `{g['image_group']:#x}/{g['image_group_size']:#x}` | "
            f"`{g['width']:#x}/{g['height']:#x}` | `{g['video_mode']:#x}/{g['mask_mode']:#x}` |")
    lines += ["", "## Provenienza e limiti", "",
        f"- {len(report['archives'])} archivi inventariati con SHA-256; "
        "il JSON associato contiene membri, hash, offset e motivi di esclusione.",
        "- Ogni riga elenca solo i campioni effettivamente presenti, anche quando "
        "la stessa geometria ricorre su release distanti.",
        "- 4.00: resta sperimentale per il disallineamento di un byte dei dati RIP-relative.",
        "- Gli RVA delle funzioni sono separati dalla geometria: possono cambiare "
        "anche quando le strutture sono uguali. Il payload risolve gli export a runtime.",
        "- L'audit non verifica SDK/kernel, arbitraggio risorse, modalità HDMI, audio, "
        "permessi o stabilità hardware: la geometria video da sola non prova il funzionamento.",
        "- La sola identità di segmenti Avcap2 non prova l'identità di RemotePlay o del servizio backend.",
        "- Non sono stati usati risultati REA: server non connesso. Analisi riproducibile "
        "con il parser Capstone locale, hash SHA-256 e lettura ELF64.", "",
        "## Selezione nel payload", "", "Questo strumento non modifica il registro di produzione. "
        "La politica di selezione per ID esatto o intervallo è implementata in "
        "generate_ps5_offsets.py e documentata in docs/PS5_FIRMWARE_RANGES.md. "
        "Abilitare un intervallo nel codice non equivale a un test hardware di tutte le sue release.", "",
        "## Prima di pubblicare un range continuo", "",
        "Servono almeno il dump 9.00 Avcap2 + RemotePlay, i campioni che delimitano "
        "i passaggi 9.xx→10.xx, 10.xx→11.xx e 12.xx→13.xx, e test di cattura "
        "per le versioni dichiarate. Gli intervalli fra campioni uguali sono candidati, "
        "non test completati.", "",
        "## Riproduzione", "", "```powershell",
        "python tools/audit_ps5_firmware.py C:/Users/marco/Downloads/PBS-Dumps `",
        "  --local-root D:/pbs/dumps `",
        "  --reference-940 ../PBS/notes/re/session-20261015/avcap2.bin `",
        "  --out build/firmware-audit",
        "```", "", "Il report non contiene binari Sony e non modifica i dump.", ""]
    details = []
    versions = {r["version"]: r for r in report["firmware"]}
    if "9.00" not in versions:
        details.append("- Nessun dump 9.00 nel perimetro analizzato: il limite inferiore 9.00 non è dimostrato.")
    if "9.40" in versions and versions["9.40"] in complete:
        details.append("- 9.40: Avcap2 e costruttore verificati indipendentemente nei dump locali; "
                       "la geometria coincide con 9.20, senza dover ereditare il suo costruttore.")
    if "11.40" in versions and "12.70" in versions and \
            versions["11.40"].get("geometry") == versions["12.70"].get("geometry"):
        details.append("- 11.40 PS5 Pro: stessa geometria della 12.70. Il modello Pro deriva "
                       "dal nome dell'archivio, non è autenticato dal disassemblato.")
    if report["plaintext_elf_comparisons"]:
        same = all(x["executable_segments_equal"] for x in report["plaintext_elf_comparisons"])
        details.append(f"- 13.40/13.60: segmenti eseguibili Avcap2 identici: {same}. "
                       "Avcap2 13.40 usa request 0x2e8/kind 0x1a8; manca il costruttore "
                       "RemotePlay 13.40, quindi resta evidenza parziale.")
    lines += ["## Risultati aggiuntivi del perimetro corrente", ""] + details + ["",
        "## Inventario archivi", "", "| Archivio | Membri memoria letti | SHA-256 |", "|---|---:|---|"]
    lines += [f"| {a['name']} | {a['memory_members']} | `{a['sha256']}` |" for a in report["archives"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dumps", type=Path)
    parser.add_argument("--local-root", type=Path, action="append", default=[])
    parser.add_argument("--reference-940", type=Path)
    parser.add_argument("--seven-zip", default=r"C:\Program Files\7-Zip\7z.exe")
    parser.add_argument("--out", type=Path, default=Path("build/firmware-audit"))
    parser.add_argument("--audit-date", default=date.today().isoformat())
    args = parser.parse_args()
    gathered, inventory = collect(args.dumps, args.local_root, args.reference_940, args.seven_zip)
    records = analyze(gathered)
    report = {"schema_version": 1, "audit_date": args.audit_date, "method": "local-capstone-static",
              "dumps_root": str(args.dumps.resolve()), "archives": inventory,
              "firmware": records, "families": families(records), "plaintext_elf_comparisons": []}
    a = args.dumps / "13.40libs.7z"
    b = args.dumps / "fw13.60.rar"
    if a.exists() and b.exists():
        base = archive_file(a, "libSceAvcap2.sprx", args.seven_zip)
        peers = archive_file(b, "libSceAvcap2.sprx", args.seven_zip)
        for source, data in base:
            memory, segments = elf_image(data)
            for other_source, other in peers:
                _, other_segments = elf_image(other)
                report["plaintext_elf_comparisons"].append({
                    "source": f"{a.name}:{source}", "sha256": sha(data),
                    "peer": f"{b.name}:{other_source}", "peer_sha256": sha(other),
                    "executable_segments": segments,
                    "executable_segments_equal": segments == other_segments,
                    "avcap_layout_1340": gen.avcap_layout(memory),
                    "remoteplay_constructor_1340": None})
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "firmware-audit.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.out / "firmware-audit.md").write_text(render(report), encoding="utf-8")
    for group in report["families"]:
        print(", ".join(group["observed_versions"]), f"request={group['geometry']['size']:#x}")
    print(f"{len(inventory)} archives; {len(records)} firmware IDs; {len(report['families'])} observed ABI families")
    for comparison in report["plaintext_elf_comparisons"]:
        print("13.40/13.60 executable segments equal:", comparison["executable_segments_equal"])
    print(args.out / "firmware-audit.md")


if __name__ == "__main__":
    main()
