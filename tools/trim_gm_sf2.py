#!/usr/bin/env python3
"""
Trim a General MIDI SoundFont down to only the presets used by the bundled
MIDI tracks (3DPB pinball.mid plus Full Tilt taba1/2/3.mds). Samples and
metadata for those presets are kept byte-for-byte, everything else is
discarded.

The web build bakes gm.sf2 into the .data bundle. A full 128-instrument
General MIDI soundfont is roughly 3 MB. The bundled tracks together touch
just seven presets, so the trimmed font lands around 600 KB. The audio is
identical, the file is just smaller.

Presets kept (bank, preset):
    3DPB pinball.mid:
        ( 0,  0)  Acoustic Grand Piano
        ( 0, 11)  Vibraphone
        ( 0, 28)  Electric Guitar (muted)
        ( 0, 38)  Synth Bass 1
        (128, 0)  Standard drum kit (channel 9)
    Full Tilt taba*.mds extras:
        ( 0,  5)  Electric Piano 2
        ( 0, 83)  Lead 4 (chiff)

Usage:
    tools/trim_gm_sf2.py path/to/full.sf2 [path/to/output.sf2]

If the output path is omitted, the trimmed font is written to
``game_resources/gm.sf2`` next to this script.

The output is deterministic. Re-run after changing the KEEP set.
"""
import argparse
import pathlib
import struct
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "game_resources" / "gm.sf2"

# (bank, preset) pairs to keep. Bank 128 is the drum kit bank in GM.
KEEP = {(0, 0), (0, 5), (0, 11), (0, 28), (0, 38), (0, 83), (128, 0)}

# SF2 generator opcodes used to navigate the preset/instrument/sample tree.
# See the SoundFont 2 spec, section 8.1.3.
INST_GEN_OPER = 41    # preset zone references an instrument
SAMPLE_GEN_OPER = 53  # instrument zone references a sample

# A modulator record is 10 bytes (sfModList in the spec). A single all-zero
# record marks the end of the chunk and is the minimum legal payload.
EMPTY_MOD_CHUNK = b"\0" * 10


def find_chunks(data):
    """Walk the RIFF tree once and return {chunkid: (offset, size)} for every
    leaf chunk. The outer file header is at offset 0, leaf chunks start past
    the RIFF/sfbk/LIST/listid headers."""
    out = {}
    stack = [(12, len(data))]
    while stack:
        o, end = stack.pop()
        while o < end:
            cid = data[o:o + 4]
            sz = struct.unpack("<I", data[o + 4:o + 8])[0]
            body = o + 8
            if cid in (b"RIFF", b"LIST"):
                stack.append((body + 4, body + sz))
            else:
                out[cid] = (body, sz)
            o = body + sz + (sz & 1)
    return out


def split_records(data, chunk, size):
    """Slice a fixed-record chunk into its individual records."""
    o, sz = chunk
    return [data[o + i * size:o + (i + 1) * size] for i in range(sz // size)]


def preset_name(record):
    return record[:20].split(b"\0", 1)[0].decode("latin1", "replace").strip()


def trim(src_bytes):
    if src_bytes[:4] != b"RIFF" or src_bytes[8:12] != b"sfbk":
        sys.exit("error: input is not an SF2 (RIFF/sfbk) file")

    chunks = find_chunks(src_bytes)

    phdr = split_records(src_bytes, chunks[b"phdr"], 38)
    pbag = split_records(src_bytes, chunks[b"pbag"], 4)
    pgen = split_records(src_bytes, chunks[b"pgen"], 4)
    inst = split_records(src_bytes, chunks[b"inst"], 22)
    ibag = split_records(src_bytes, chunks[b"ibag"], 4)
    igen = split_records(src_bytes, chunks[b"igen"], 4)
    shdr = split_records(src_bytes, chunks[b"shdr"], 46)
    smpl_o, smpl_sz = chunks[b"smpl"]
    smpl = src_bytes[smpl_o:smpl_o + smpl_sz]

    keep_pi = []
    for i in range(len(phdr) - 1):  # the last record is the EOP terminal
        preset, bank = struct.unpack("<HH", phdr[i][20:24])
        if (bank, preset) in KEEP:
            keep_pi.append(i)

    found = {struct.unpack("<HH", phdr[i][20:24])[::-1] for i in keep_pi}
    missing = KEEP - found
    if missing:
        sys.exit(f"error: presets not found in source soundfont: {sorted(missing)}")
    print(f"keeping {len(keep_pi)} presets: "
          + ", ".join(preset_name(phdr[i]) for i in keep_pi))

    # Preset zones reference instruments via the INST generator.
    keep_inst = set()
    for pi in keep_pi:
        bag_lo = struct.unpack("<H", phdr[pi][24:26])[0]
        bag_hi = struct.unpack("<H", phdr[pi + 1][24:26])[0]
        for bi in range(bag_lo, bag_hi):
            g_lo = struct.unpack("<H", pbag[bi][0:2])[0]
            g_hi = struct.unpack("<H", pbag[bi + 1][0:2])[0]
            for gi in range(g_lo, g_hi):
                oper, amount = struct.unpack("<HH", pgen[gi])
                if oper == INST_GEN_OPER:
                    keep_inst.add(amount)

    # Instrument zones reference samples via the sampleID generator.
    keep_smp = set()
    for ii in keep_inst:
        bag_lo = struct.unpack("<H", inst[ii][20:22])[0]
        bag_hi = struct.unpack("<H", inst[ii + 1][20:22])[0]
        for bi in range(bag_lo, bag_hi):
            g_lo = struct.unpack("<H", ibag[bi][0:2])[0]
            g_hi = struct.unpack("<H", ibag[bi + 1][0:2])[0]
            for gi in range(g_lo, g_hi):
                oper, amount = struct.unpack("<HH", igen[gi])
                if oper == SAMPLE_GEN_OPER:
                    keep_smp.add(amount)

    # Pull in stereo-linked partners. Sample types 2, 4, and 8 are
    # right/left/linked in the SF2 spec.
    for si in list(keep_smp):
        link = struct.unpack("<H", shdr[si][42:44])[0]
        stype = struct.unpack("<H", shdr[si][44:46])[0]
        if stype in (2, 4, 8) and link < len(shdr) - 1:
            keep_smp.add(link)

    print(f"  -> {len(keep_inst)} instruments, {len(keep_smp)} samples "
          f"(of {len(inst) - 1} / {len(shdr) - 1})")

    # Rebuild smpl + shdr, renumbering kept samples and recomputing offsets.
    PAD_FRAMES = 46  # SF2 spec requires at least 46 zero frames between samples
    keep_smp_sorted = sorted(keep_smp)
    smp_remap = {old: new for new, old in enumerate(keep_smp_sorted)}
    new_smpl = bytearray()
    new_shdr = bytearray()
    for old_i in keep_smp_sorted:
        r = shdr[old_i]
        start, end, sloop, eloop, rate = struct.unpack("<IIIII", r[20:40])
        base = len(new_smpl) // 2
        new_smpl += smpl[start * 2:end * 2]
        new_smpl += b"\0" * (PAD_FRAMES * 2)
        new_start = base
        new_end = base + (end - start)
        new_sloop = base + (sloop - start)
        new_eloop = base + (eloop - start)
        new_shdr += r[:20] + struct.pack("<IIIII", new_start, new_end,
                                          new_sloop, new_eloop, rate) + r[40:46]

    # Second pass to remap sampleLink in the rebuilt headers.
    tmp = bytearray()
    for new_i in range(len(keep_smp_sorted)):
        rec = bytearray(new_shdr[new_i * 46:(new_i + 1) * 46])
        link = struct.unpack("<H", rec[42:44])[0]
        stype = struct.unpack("<H", rec[44:46])[0]
        new_link = smp_remap.get(link, 0) if stype in (2, 4, 8) else 0
        rec[42:44] = struct.pack("<H", new_link)
        tmp += rec
    new_shdr = bytes(tmp) + b"EOS".ljust(20, b"\0") + b"\0" * 26

    # Rebuild inst/ibag/igen for the kept instruments.
    keep_inst_sorted = sorted(keep_inst)
    inst_remap = {old: new for new, old in enumerate(keep_inst_sorted)}
    new_inst = bytearray()
    new_ibag = bytearray()
    new_igen = bytearray()
    for old_ii in keep_inst_sorted:
        new_inst += inst[old_ii][:20] + struct.pack("<H", len(new_ibag) // 4)
        bag_lo = struct.unpack("<H", inst[old_ii][20:22])[0]
        bag_hi = struct.unpack("<H", inst[old_ii + 1][20:22])[0]
        for bi in range(bag_lo, bag_hi):
            # Modulator index 0 references the terminal record in EMPTY_MOD_CHUNK.
            new_ibag += struct.pack("<HH", len(new_igen) // 4, 0)
            g_lo = struct.unpack("<H", ibag[bi][0:2])[0]
            g_hi = struct.unpack("<H", ibag[bi + 1][0:2])[0]
            for gi in range(g_lo, g_hi):
                oper, amount = struct.unpack("<HH", igen[gi])
                if oper == SAMPLE_GEN_OPER:
                    amount = smp_remap[amount]
                new_igen += struct.pack("<HH", oper, amount)
    new_inst += b"EOI".ljust(20, b"\0") + struct.pack("<H", len(new_ibag) // 4)
    new_ibag += struct.pack("<HH", len(new_igen) // 4, 0)
    new_igen += struct.pack("<HH", 0, 0)

    # Rebuild phdr/pbag/pgen for the kept presets.
    new_phdr = bytearray()
    new_pbag = bytearray()
    new_pgen = bytearray()
    for old_pi in keep_pi:
        new_phdr += (phdr[old_pi][:24]
                     + struct.pack("<H", len(new_pbag) // 4)
                     + phdr[old_pi][26:38])
        bag_lo = struct.unpack("<H", phdr[old_pi][24:26])[0]
        bag_hi = struct.unpack("<H", phdr[old_pi + 1][24:26])[0]
        for bi in range(bag_lo, bag_hi):
            new_pbag += struct.pack("<HH", len(new_pgen) // 4, 0)
            g_lo = struct.unpack("<H", pbag[bi][0:2])[0]
            g_hi = struct.unpack("<H", pbag[bi + 1][0:2])[0]
            for gi in range(g_lo, g_hi):
                oper, amount = struct.unpack("<HH", pgen[gi])
                if oper == INST_GEN_OPER:
                    amount = inst_remap[amount]
                new_pgen += struct.pack("<HH", oper, amount)
    new_phdr += (b"EOP".ljust(20, b"\0")
                 + struct.pack("<HHH", 0, 0, len(new_pbag) // 4)
                 + b"\0" * 12)
    new_pbag += struct.pack("<HH", len(new_pgen) // 4, 0)
    new_pgen += struct.pack("<HH", 0, 0)

    # Copy the INFO list verbatim from the source so credits and version
    # strings are preserved.
    info_blob = b""
    o = 12
    while o < len(src_bytes):
        cid = src_bytes[o:o + 4]
        sz = struct.unpack("<I", src_bytes[o + 4:o + 8])[0]
        if cid == b"LIST" and src_bytes[o + 8:o + 12] == b"INFO":
            info_blob = src_bytes[o:o + 8 + sz + (sz & 1)]
            break
        o += 8 + sz + (sz & 1)

    def chunk(cid, data):
        out = cid + struct.pack("<I", len(data)) + bytes(data)
        if len(data) & 1:
            out += b"\0"
        return out

    def listchunk(typ, *chunks):
        body = typ + b"".join(chunks)
        return b"LIST" + struct.pack("<I", len(body)) + body

    sdta = listchunk(b"sdta", chunk(b"smpl", new_smpl))
    pdta = listchunk(
        b"pdta",
        chunk(b"phdr", new_phdr),
        chunk(b"pbag", new_pbag),
        chunk(b"pmod", EMPTY_MOD_CHUNK),
        chunk(b"pgen", new_pgen),
        chunk(b"inst", new_inst),
        chunk(b"ibag", new_ibag),
        chunk(b"imod", EMPTY_MOD_CHUNK),
        chunk(b"igen", new_igen),
        chunk(b"shdr", new_shdr),
    )
    body = b"sfbk" + info_blob + sdta + pdta
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Trim a General MIDI SoundFont to the presets the bundled MIDI tracks use.",
    )
    parser.add_argument(
        "input",
        type=pathlib.Path,
        help="Path to a full GM .sf2 (e.g. FluidR3_GM, GeneralUser GS, or any GM-128 bank).",
    )
    parser.add_argument(
        "output",
        nargs="?",
        type=pathlib.Path,
        default=DEFAULT_OUT,
        help=f"Output path. Default: {DEFAULT_OUT.relative_to(REPO_ROOT)}",
    )
    args = parser.parse_args(argv)

    if not args.input.is_file():
        sys.exit(f"error: input file not found: {args.input}")

    src_bytes = args.input.read_bytes()
    out_bytes = trim(src_bytes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out_bytes)

    src_kb = len(src_bytes) / 1024
    out_kb = len(out_bytes) / 1024
    pct = 100.0 * len(out_bytes) / len(src_bytes)
    print()
    print(f"{src_kb:8.1f} KB  {args.input}")
    print(f"{out_kb:8.1f} KB  {args.output}")
    print(f"  -> {pct:.1f}% of original ({src_kb - out_kb:.0f} KB saved)")


if __name__ == "__main__":
    main()
