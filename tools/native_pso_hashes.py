"""Write <spirv>/native_hashes.txt: the HASH digest of each carved DXBC container -> its stage and path.

Xbox device slot 57 creates the game's graphics pipelines from native code, and each stage's records name the
source container by the 16-byte digest of its HASH part (docs/fspx.md, kind 7; issue #7049). The runtime
(runtime/src/native_pso.h) reads this file to find the container and gives it to CreateGraphicsPipelineState,
which finds the translated module by fssm_sha256 as for a PC title.

Usage: python -I tools/native_pso_hashes.py <work>/shaders/dxbc <work>/shaders/spirv

Only containers in the translator's index are written. Two containers with one digest must have the same
fssm_sha256 (the 1.0.2.1 corpus: 25,321 digests over 26,503 containers, 789 shared, none with two programs);
otherwise the step fails and writes nothing. Standard library only; no game data.
"""
import json
import os
import struct
import sys

HEADER = "# native_pso_hashes 1"
OUT_NAME = "native_hashes.txt"


class DigestClash(ValueError):
    """One digest names two different programs."""


def hash_digest(data: bytes):
    """The 16-byte digest of a DXBC container's HASH part (after its u32 flags), or None."""
    if len(data) < 32 or data[:4] != b"DXBC":
        return None
    count = struct.unpack_from("<I", data, 28)[0]
    if 32 + 4 * count > len(data):
        return None
    for i in range(count):
        offset = struct.unpack_from("<I", data, 32 + 4 * i)[0]
        if offset + 8 > len(data):
            return None
        fourcc, size = data[offset:offset + 4], struct.unpack_from("<I", data, offset + 4)[0]
        if fourcc == b"HASH":
            if size < 20 or offset + 8 + size > len(data):
                return None
            return data[offset + 12:offset + 28]
    return None


def build(dxbc: str, spirv: str) -> dict:
    """{digest hex: (stage, path relative to spirv)}. Raises DigestClash when one digest names two programs."""
    with open(os.path.join(spirv, "index.json"), encoding="utf-8") as f:
        index = json.load(f)
    if not isinstance(index, dict):
        raise ValueError("index.json is not an object")
    found = {}
    for name in sorted(os.listdir(dxbc)):
        if not name.endswith(".dxbc"):
            continue
        stem = name[:-5]
        entry = index.get(stem)
        if not isinstance(entry, dict) or "\t" in stem or "\n" in stem:
            continue
        stage, fssm = entry.get("stage"), entry.get("fssm_sha256")
        if not isinstance(stage, str) or not isinstance(fssm, str):
            continue
        with open(os.path.join(dxbc, name), "rb") as f:
            digest = hash_digest(f.read())
        if digest is None:
            continue
        key = digest.hex()
        path = os.path.relpath(os.path.join(dxbc, name), spirv).replace(os.sep, "/")
        if key in found:
            first = found[key]
            if first[2] != fssm or first[0] != stage:
                raise DigestClash(f"digest {key} names two programs: {first[3]} and {stem}")
            continue
        found[key] = (stage, path, fssm, stem)
    return {k: (v[0], v[1]) for k, v in found.items()}


def write(spirv: str, table: dict) -> str:
    out = os.path.join(spirv, OUT_NAME)
    tmp = out + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + "\n")
        for key in sorted(table):
            stage, path = table[key]
            f.write(f"{key}\t{stage}\t{path}\n")
    os.replace(tmp, out)
    return out


def main(argv) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: native_pso_hashes.py <dxbc dir> <spirv dir>", file=sys.stderr)
        return 2
    dxbc, spirv = argv[1], argv[2]
    try:
        table = build(dxbc, spirv)
    except (OSError, ValueError) as e:
        print(f"native_pso_hashes: {e}", file=sys.stderr)
        stale = os.path.join(spirv, OUT_NAME)
        if os.path.exists(stale):
            os.remove(stale)
        return 1
    out = write(spirv, table)
    print(f"native_pso_hashes: {len(table)} digests -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
