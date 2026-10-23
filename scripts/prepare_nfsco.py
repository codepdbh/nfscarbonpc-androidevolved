#!/usr/bin/env python3
"""Prepare the NFSCO online mod (nfsco.club) as lifted guest code.

  prepare_nfsco.py <capture dir>        (tools/nfsco_capture.exe output, e.g. work/nfsco_patches_v3)

NFSCO's palmont.dll patches NFSC.exe in memory (OnAttach): jumps into itself
and into trampolines it allocates, plus instruction and data edits. Lifted
code cannot follow bytes written at run time, so the capture made on Windows
with the guest layout (palmont 0x01060000, trampoline pool 0x01200000,
logging.dll 0x01240000) is turned into:

  work/nfsco/nfsco.analysis.exe   NFSC.exe's analysis copy with every captured
                                  patch applied; it replaces work/speed.analysis.exe
                                  as the lift input (src/recomp/gen)
  src/recomp/gen_palmont          palmont.dll relocated to 0x01060000
  src/recomp/gen_logging          logging.dll relocated to 0x01240000
  src/recomp/gen_pool             the trampoline pool (64-byte slots, MinHook
                                  layout) as code at 0x01200000

At run time palmont's OnAttach runs translated and writes the same patches;
its executable allocations are served from the pool in the captured order.
The NFSCO authors allowed translating their binaries for this port; nothing
here is committed (work/ and src/recomp/gen_* are gitignored).
"""
import json
import os
import pathlib
import struct
import subprocess
import sys

import pefile

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAME = pathlib.Path(os.environ.get("NFSC_GAME_ROOT", ROOT.parent / "Need for Speed Carbon"))
WORK = ROOT / "work" / "nfsco"
POOL_BASE, POOL_SIZE, SLOT = 0x01200000, 0x40000, 64
MODULES = {  # name: (source, guest base, prefix)
    "palmont.dll": (GAME / "NFSCO" / "bin" / "palmont.dll", 0x01060000, "palmont_"),
    "logging.dll": (GAME / "logging.dll", 0x01240000, "logging_"),
    # ThirteenAG's WidescreenFix (MIT), loaded by the ASI loader next to NFSCO
    # on PC: camera/FOV, HUD and front-end placement at any aspect ratio.
    "NFSCarbon.WidescreenFix.asi": (GAME / "scripts" / "NFSCarbon.WidescreenFix.asi", 0x01300000, "wsfix_"),
}
PLUGIN_TAGS = ("palmont", "pool", "wsfix")


def run(script, *arguments):
    subprocess.run([sys.executable, str(ROOT / "scripts" / script), *(str(a) for a in arguments)], cwd=ROOT, check=True)


def read_patches(capture):
    """(va, patched bytes, [kind, target]); the bytes come from image.bin (the
    report truncates long ranges), checked against the report's prefix."""
    image = (capture / "image.bin").read_bytes()
    out = []
    for line in (capture / "patches.txt").read_text().splitlines():
        parts = line.split()
        va, size, shown = int(parts[0], 16), int(parts[1]), bytes.fromhex(parts[4])
        new = image[va - 0x400000:va - 0x400000 + size]
        if len(new) != size or not new.startswith(shown):
            sys.exit(f"patch does not match image.bin: {line}")
        out.append((va, new, parts[5:]))
    return out


def patched_exe(capture):
    source = ROOT / "work" / "speed.analysis.exe"
    pe = pefile.PE(str(source), fast_load=True)
    base, data = pe.OPTIONAL_HEADER.ImageBase, bytearray(source.read_bytes())
    applied = skipped = 0
    for va, new, _ in read_patches(capture):
        try:
            off = pe.get_offset_from_rva(va - base)
        except pefile.PEFormatError:
            off = None
        section = pe.get_section_by_rva(va - base)
        if off is None or section is None or va - base + len(new) > section.VirtualAddress + section.SizeOfRawData:
            skipped += 1   # uninitialized data: palmont writes it at run time
            continue
        data[off:off + len(new)] = new
        applied += 1
    out = WORK / "nfsco.analysis.exe"
    out.write_bytes(bytes(data))
    print(f"[*] {applied} patches applied to the analysis copy, {skipped} in uninitialized data -> {out}")
    return out


def pool_module(capture):
    """The trampoline pool as a minimal PE32 at POOL_BASE; seeds: every used slot."""
    code = (capture / "pool.bin").read_bytes()
    if len(code) > POOL_SIZE:
        sys.exit("pool larger than its reservation")
    # MinHook's layout: 64-byte trampoline slots in the first block (0x20000
    # bytes), 32-byte relay slots in the next one.
    seeds = [{"address": POOL_BASE + i, "note": "nfsco pool slot"} for i in range(0, min(len(code), 0x20000), SLOT) if any(code[i:i + SLOT])]
    seeds += [{"address": POOL_BASE + i, "note": "nfsco pool relay"} for i in range(0x20000, len(code), 32) if any(code[i:i + 32])]
    targets = {int(p[2][-1], 16) for p in read_patches(capture) if len(p[2]) == 2 and p[2][0] in ("jmp", "call")}
    missing = [hex(t) for t in targets if POOL_BASE <= t < POOL_BASE + len(code) and (t - POOL_BASE) % (SLOT if t < POOL_BASE + 0x20000 else 32)]
    if missing:
        sys.exit(f"patch targets inside pool slots not at a slot start: {missing}")
    text_rva, size = 0x1000, len(code)
    image_size = text_rva + ((size + 0xFFF) & ~0xFFF)
    hdr = bytearray(0x1000)
    struct.pack_into("<2s58xI", hdr, 0, b"MZ", 0x40)
    struct.pack_into("<4sHHIIIHH", hdr, 0x40, b"PE\0\0", 0x14C, 1, 0, 0, 0, 0xE0, 0x2102)
    struct.pack_into("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", hdr, 0x58, 0x10B, 1, 0, size, 0, 0, 0, text_rva, 0,
                     POOL_BASE - text_rva, 0x1000, 0x1000, 4, 0, 0, 0, 4, 0, 0, image_size, 0x1000, 0, 2, 0,
                     0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    struct.pack_into("<8sIIIIIIHHI", hdr, 0x58 + 0xE0, b".text", size, text_rva, size, 0x1000, 0, 0, 0, 0, 0x60000020)
    dll = WORK / "pool.analysis.dll"
    dll.write_bytes(bytes(hdr) + code)
    seeds_path = WORK / "pool_seeds.json"
    json.dump(seeds, open(seeds_path, "w"), indent=1)
    print(f"[*] pool: {len(code):#x} bytes, {len(seeds)} used slots -> {dll}")
    return dll, seeds_path


CAPTURE = None


def main(capture):
    global CAPTURE
    capture = pathlib.Path(capture).resolve()
    CAPTURE = capture
    WORK.mkdir(parents=True, exist_ok=True)
    modules = (capture / "modules.txt").read_text()
    for need in ("palmont.dll 01060000", "logging.dll 01240000", "pool 01200000", "widescreenfix 01300000"):
        if need not in modules:
            sys.exit(f"capture was not made with the guest layout ({need} missing in modules.txt)")
    patched_exe(capture)
    dll, seeds = pool_module(capture)
    # Every slot is forced: the zero padding after a slot's final jump decodes
    # as code and would otherwise hide the next slot from the seed rules.
    forced = WORK / "pool_forced.json"
    json.dump([e["address"] for e in json.load(open(seeds))], open(forced, "w"))
    run("run_lift.py", "catalog", dll, seeds, WORK / "pool_catalog.json", "--seeds-only", "--forced", forced)
    run("run_lift.py", "lift", dll, WORK / "pool_catalog.json", ROOT / "src/recomp/gen_pool", "--prefix", "pool_")
    for name, (source, base, prefix) in MODULES.items():
        tag = prefix.rstrip("_")
        image, seeds = WORK / f"{tag}.relocated.dll", WORK / f"{tag}_seeds.json"
        run("prepare_module.py", source, hex(base), image, seeds)
        catalog = WORK / f"{tag}_catalog.json"
        run("run_lift.py", "catalog", image, seeds, catalog, "--seeds-only")
        # Seeds the rules rejected because a jump table decoded as code covers
        # them, although they open with an MSVC frame prologue: forced.
        data = pefile.PE(str(image)).get_memory_mapped_image()
        starts = {f["address"] for f in json.load(open(catalog))["functions"]}
        prologue = []
        for e in json.load(open(seeds)):
            va = e["address"] if isinstance(e, dict) else e
            head = data[va - base:va - base + 5]
            if va not in starts and (head[:3] == b"\x55\x8b\xec" or head == b"\x8b\xff\x55\x8b\xec"):
                prologue.append(va)
        print(f"[*] {name}: {len(prologue)} rejected seeds open a frame prologue: forced")
        if prologue:
            forced = WORK / f"{tag}_forced.json"
            json.dump(prologue, open(forced, "w"))
            run("run_lift.py", "catalog", image, seeds, catalog, "--seeds-only", "--forced", forced)
        run("run_lift.py", "lift", image, WORK / f"{tag}_catalog.json", ROOT / "src/recomp" / f"gen_{tag}", "--prefix", prefix)
    lift_game()


def continuations():
    """Game addresses palmont's hooks and trampolines return to with
    `push imm32; ret` (lift32 turns that into a jump): each must be a lifted
    entry of the patched NFSC.exe, although it is the middle of a function."""
    import re
    found = set()
    for tag in PLUGIN_TAGS:
        for c in (ROOT / "src/recomp" / f"gen_{tag}").glob(f"{tag}_recomp_*.c"):
            # push/ret jumps, and every direct jump or call into the game
            # (trampolines resume the original function after its prologue).
            for m in re.finditer(r"RECOMP_I(?:TAIL|CALL)\(0x([0-9A-F]{8})u\)", c.read_text()):
                va = int(m.group(1), 16)
                if 0x401000 <= va < 0x9C1000:
                    found.add(va)
    return sorted(found)


def game_pointers():
    """Game functions palmont and its trampolines reach through pointers
    (call eax, call [table]): every 32-bit value in their images that lands in
    NFSC.exe's code and probes as a function body. The catalog rejects values
    that fall inside code it already decoded."""
    sys.path.insert(0, str(ROOT.parent / "pcrecomp" / "tools" / "pe"))
    sys.path.insert(0, str(ROOT.parent / "pcrecomp" / "tools" / "disasm"))
    from pe_analyze import analyze_pe
    from disasm32 import Disassembler
    exe = WORK / "nfsco.analysis.exe"
    info = analyze_pe(str(exe))
    dis = Disassembler(exe.read_bytes(), info.image_base, info.sections)
    import re
    found = set()
    # Immediates of instructions the lifter decoded (mov eax, imm; push imm; ...).
    for tag in PLUGIN_TAGS:
        for c in (ROOT / "src/recomp" / f"gen_{tag}").glob(f"{tag}_recomp_*.c"):
            for m in re.finditer(r"0x00([4-9][0-9A-F]{5})u", c.read_text()):
                found.add(int(m.group(1), 16))
    # Aligned values in palmont's data sections (function pointer tables).
    for image in ("palmont", "wsfix"):
      pe = pefile.PE(str(WORK / f"{image}.relocated.dll"))
      for section in pe.sections:
        if section.Characteristics & 0x20000000:   # executable: covered above
            continue
        data = section.get_data()
        for off in range(0, len(data) - 3, 4):
            found.add(int.from_bytes(data[off:off + 4], "little"))
    found = {va for va in found if 0x401000 <= va < 0x9C1000}
    return sorted(va for va in found if dis.probes_as_function_body(va))


def lift_game():
    """Catalog and lift the patched NFSC.exe into src/recomp/gen.

    Pass 1: the usual code seeds, with the hook continuations as forced roots
    (they are the middle of game functions, which the seed rules reject).
    Pass 2: also the game addresses palmont points at that fall in gaps of
    pass 1 (outside every catalogued function), forced too, so no catalogued
    function is displaced by a stray value."""
    seeds = ROOT / "work" / "seeds_code.json"
    exe, catalog = WORK / "nfsco.analysis.exe", WORK / "catalog.json"
    extra = continuations()
    forced = WORK / "nfsc_forced.json"
    json.dump(extra, open(forced, "w"))
    print(f"[*] {len(extra)} hook continuations forced into the NFSC.exe catalog")
    run("run_lift.py", "catalog", exe, seeds, catalog, "--forced", forced)
    functions = json.load(open(catalog))["functions"]
    extents = sorted((f["address"], f["end"]) for f in functions)
    import bisect
    starts = [a for a, _ in extents]

    def covered(va):
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and extents[i][0] <= va < extents[i][1]
    gaps = [va for va in game_pointers() if not covered(va)]
    print(f"[*] {len(gaps)} game addresses palmont points at lie outside the catalog: forced too")
    if gaps:
        json.dump(extra + gaps, open(forced, "w"))
        run("run_lift.py", "catalog", exe, seeds, catalog, "--forced", forced)
    starts = {f["address"] for f in json.load(open(catalog))["functions"]}
    missing = [hex(va) for va in extra + gaps if va not in starts]
    if missing:
        sys.exit(f"forced roots missing from the catalog: {missing}")
    captured = patch_sites(CAPTURE, catalog)
    late_sites, variants = runtime_patches(catalog, set(captured))
    # A variant may continue where the lifted bytes ended (a hook palmont
    # removes again: the lifted function is only its jmp): its join must be a
    # lifted entry too.
    joins = [v[1] for v in variants if v[1] not in starts]
    if joins:
        print(f"[*] {len(joins)} variant joins forced into the NFSC.exe catalog")
        json.dump(extra + gaps + joins, open(forced, "w"))
        run("run_lift.py", "catalog", exe, seeds, catalog, "--forced", forced)
        captured = patch_sites(CAPTURE, catalog)
        late_sites, variants = runtime_patches(catalog, set(captured))
    sites = WORK / "patch_sites.json"
    json.dump(sorted(set(captured) | late_sites), open(sites, "w"))
    variants_path = WORK / "variants.json"
    json.dump(variants, open(variants_path, "w"), indent=0)
    out = ROOT / "src/recomp/gen"
    if out.exists():
        import shutil
        shutil.rmtree(out)
    run("run_lift.py", "lift", exe, catalog, out, "--patch-sites", sites, "--variants", variants_path)


def runtime_patches(catalog, sites):
    """Code palmont rewrites after its OnAttach (online race setup and the like)
    and restores later, measured from dumps of the running game's .text
    (work/nfsco/runtime_code/*.bin: {u32 address, u32 size, bytes} records, as
    the runtime's online trace writes them). Returns (extra patch sites,
    variants): a change confined to an instruction's trailing operand is read
    from memory; any other change becomes a lift32 variant that runs the
    dumped bytes when memory differs from the lifted ones."""
    import bisect
    import struct
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    exe = pefile.PE(str(WORK / "nfsco.analysis.exe"), fast_load=True)
    base = exe.get_memory_mapped_image()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    blocks = sorted((b[0], b[1]) for f in json.load(open(catalog))["functions"] for b in f["blocks"])
    starts = [b[0] for b in blocks]

    def block_of(va):
        i = bisect.bisect_right(starts, va) - 1
        return blocks[i] if i >= 0 and blocks[i][0] <= va < blocks[i][1] else None
    dumped = {}
    for path in sorted((WORK / "runtime_code").glob("*.bin")):
        data, at = path.read_bytes(), 0
        while at + 8 <= len(data):
            va, size = struct.unpack_from("<II", data, at)
            at += 8
            for k in range(size):
                if 0x401000 <= va + k < 0x9C1000 and data[at + k] != base[va + k - 0x400000]:
                    dumped[va + k] = data[at + k]
            at += size
    ranges = []
    for va in sorted(dumped):
        if ranges and va <= ranges[-1][1] + 1:
            ranges[-1][1] = va
        else:
            ranges.append([va, va])
    extra_sites, variants = set(), {}
    for first, last in ranges:
        if any(s <= first < s + 4 for s in sites):
            continue
        block = block_of(first)
        if not block:
            continue          # data kept in .text (the resolution table): read from memory anyway
        insns = list(md.disasm(base[block[0] - 0x400000:block[1] - 0x400000], block[0]))
        owner = next((i for i in insns if i.address <= first < i.address + i.size), None)
        if owner is None:
            continue
        end = owner.address + owner.size
        relative = owner.bytes[0] in (0xE8, 0xE9) or (owner.bytes[0] == 0x0F and 0x80 <= owner.bytes[1] <= 0x8F)
        if last < end and first >= end - 4 and owner.size >= 5 and not relative:
            extra_sites.add(end - 4)
            continue
        other = bytearray(base[owner.address - 0x400000:owner.address - 0x400000 + 64])
        for k in range(len(other)):
            other[k] = dumped.get(owner.address + k, other[k])
        base_bounds = {i.address + i.size for i in md.disasm(base[owner.address - 0x400000:owner.address - 0x400000 + 64], owner.address)}
        join = None
        for i in md.disasm(bytes(other), owner.address):
            stop = i.address + i.size
            if stop > last and stop in base_bounds:
                join = stop
                break
            if i.mnemonic in ("ret", "jmp") and stop > last:
                join = stop
                break
        if join is None:
            sys.exit(f"runtime patch at {first:#x} does not realign with the lifted code")
        variants[owner.address] = [owner.address, join, base[owner.address - 0x400000:join - 0x400000].hex(),
                                   bytes(other[:join - owner.address]).hex()]
    print(f"[*] runtime patches from {len(list((WORK / 'runtime_code').glob('*.bin')))} dumps: "
          f"{len(extra_sites)} operands read from memory, {len(variants)} code variants")
    return extra_sites, list(variants.values())


def patch_sites(capture, catalog):
    """Code operands the plugins rewrite at run time: every captured patch
    inside an instruction's trailing imm32/disp32 (WidescreenFix stores the
    display's aspect and resolution there; palmont its constants). The lifted
    instruction reads that dword from memory, so values computed on the phone
    reach it."""
    import bisect
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    exe = pefile.PE(str(WORK / "nfsco.analysis.exe"), fast_load=True)
    image = exe.get_memory_mapped_image()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    blocks = sorted((b[0], b[1]) for f in json.load(open(catalog))["functions"] for b in f["blocks"])
    starts = [b[0] for b in blocks]
    sites = set()
    for va, new, kind in read_patches(capture):
        if kind and kind[0] in ("jmp", "call") or not 0x401000 <= va < 0x9C1000:
            continue
        i = bisect.bisect_right(starts, va) - 1
        if i < 0 or not blocks[i][0] <= va < blocks[i][1]:
            continue
        start, end = blocks[i]
        for insn in md.disasm(image[start - 0x400000:end - 0x400000], start):
            last = insn.address + insn.size
            if insn.address <= va < last:
                if insn.size >= 5 and va + len(new) <= last and va >= last - 4 - 3:
                    sites.add(last - 4)
                break
    print(f"[*] {len(sites)} plugin-written code operands read from memory at run time")
    return sorted(sites)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
