import subprocess
import argparse
import json
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--obj", action="append", required=True)
parser.add_argument("--nm", required=True)
parser.add_argument("--out", required=True)
args = parser.parse_args()

defined = set()
undefined = set()

for objfile in args.obj:
    proc = subprocess.run(
        [args.nm, "--demangle", "--defined-only", "-g", objfile],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        print(f"nm failed on {objfile}: {proc.stderr}", file=sys.stderr)
        continue

    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        # typical: "0000000000000000 T mongo::foo()"
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        _addr, symtype, name = parts
        if symtype in ("T", "D", "B", "R", "S", "V", "W"):
            if name.startswith("mongo::"):
                defined.add(name)

for objfile in args.obj:
    proc = subprocess.run(
        [args.nm, "--demangle", "--undefined-only", objfile],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        print(f"nm failed on {objfile}: {proc.stderr}", file=sys.stderr)
        continue

    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        # typical: "                 U mongo::bar()"
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        _u, name = parts
        if name.startswith("mongo::"):
            undefined.add(name)

with open(args.out, "w") as f:
    json.dump(
        {
            "defined": sorted(defined),
            "undefined": sorted(undefined),
        },
        f,
    )
