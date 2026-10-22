#!/usr/bin/env python3
"""Make an ANALYSIS copy of a game DLL relocated to its fixed guest base, plus seeds.

  prepare_module.py <game.dll> <base> <analysis.dll> <seeds.json>

Lifted code bakes absolute addresses, so a DLL is lifted as if loaded at the
base the runtime maps it at (server.dll: 0x00A00000, between SPEED2.EXE's image
and the first heap arena on both ABIs). The copy has every base relocation
applied for that base; the runtime applies the same relocations to the
original file at load time. Seeds: the entry point, every export and every
relocated pointer that lands in code (vtables, callbacks, thread starts and
jump-table targets). Both outputs are derived from the game binary: write
them under ../work (never committed). The original file is never written.
"""
import json, sys, pefile


def relocate_file(src, base, dst):
    """Copy `src` with every HIGHLOW base relocation applied for `base` and the
    header's ImageBase changed: no other byte moves. pefile's relocate_image +
    write left d3dx9_26.dll (imports merged into .text) with an import
    directory pefile itself then rejected, so the IAT went unnamed."""
    pe = pefile.PE(src)
    data = bytearray(open(src, "rb").read())
    delta = (base - pe.OPTIONAL_HEADER.ImageBase) & 0xFFFFFFFF
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for entry in block.entries:
            if entry.type != 3:  # IMAGE_REL_BASED_HIGHLOW
                continue
            off = pe.get_offset_from_rva(entry.rva)
            v = int.from_bytes(data[off:off + 4], "little")
            data[off:off + 4] = ((v + delta) & 0xFFFFFFFF).to_bytes(4, "little")
    off = pe.OPTIONAL_HEADER.get_file_offset() + 28   # ImageBase in IMAGE_OPTIONAL_HEADER32
    data[off:off + 4] = base.to_bytes(4, "little")
    open(dst, "wb").write(bytes(data))


def main(src, base, dst, seeds_path):
    relocate_file(src, base, dst)

    pe = pefile.PE(dst)
    code = [s for s in pe.sections if s.Characteristics & 0x20000000]
    def in_code(va):
        return any(base + s.VirtualAddress <= va < base + s.VirtualAddress + s.Misc_VirtualSize for s in code)
    seeds = [{"address": base + pe.OPTIONAL_HEADER.AddressOfEntryPoint, "note": "entry"}]
    for e in getattr(getattr(pe, "DIRECTORY_ENTRY_EXPORT", None), "symbols", []):  # an ASI plugin exports nothing
        seeds.append({"address": base + e.address, "note": "export " + (e.name or b"#%d" % e.ordinal).decode()})
    image = pe.get_memory_mapped_image()
    pointers = set()
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for entry in block.entries:
            if entry.type != 3:  # IMAGE_REL_BASED_HIGHLOW
                continue
            value = int.from_bytes(image[entry.rva:entry.rva + 4], "little")
            if in_code(value):
                pointers.add(value)
    seeds += [{"address": va, "note": "relocated code pointer"} for va in sorted(pointers)]
    json.dump(seeds, open(seeds_path, "w"), indent=1)
    print(f"[*] {src}: base {pefile.PE(src, fast_load=True).OPTIONAL_HEADER.ImageBase:#x} -> {base:#x}, {len(seeds)} seeds ({len(pointers)} code pointers) -> {dst}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2], 0), sys.argv[3], sys.argv[4])
