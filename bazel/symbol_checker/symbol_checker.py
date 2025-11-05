import sys
import argparse
import json
import os

parser = argparse.ArgumentParser()
parser.add_argument("--sym", required=True)
parser.add_argument("--dep", action="append")
parser.add_argument("--out", required=True)
parser.add_argument("--skip", action="store_true")
parser.add_argument("--label", required=True)
args = parser.parse_args()


# Helper to write JSON to the output file
def write_json(obj):
    with open(args.out, "w") as f:
        json.dump(obj, f, indent=2, sort_keys=True)


if args.skip:
    payload = {
        "status": "skipped",
        "target": args.label,
        "sym_file": args.sym,
        "missing": [],
        "reason": "skip tag present",
    }
    write_json(payload)
    sys.exit(0)

with open(args.sym) as f:
    current = json.load(f)

undefined = current["undefined"]
defined = set()

# we include self because we scanned all objs that belong to this library/archive
defined.update(current["defined"])

for dep_path in args.dep or []:
    with open(dep_path) as f:
        dep_sym = json.load(f)
    defined.update(dep_sym["defined"])

missing = [u for u in undefined if u not in defined]

display_name = args.label
if display_name.endswith("_with_debug"):
    display_name = display_name[: -len("_with_debug")]

if display_name.startswith("@@"):
    display_name = display_name[2:]

if missing:
    # human-readable to stderr
    header = f"Symbol check failed for: {display_name}\n"
    sys.stderr.write(header)
    sys.stderr.write("  undefined but not found in self or deps:\n")
    for u in missing:
        sys.stderr.write(f"    - {u}\n")
    sys.stderr.write(
        f"Please check to see if {display_name} is missing any deps that would include the symbols above\n"
    )

    # machine-readable to file
    payload = {
        "status": "failed",
        "target": display_name,
        "sym_file": args.sym,
        "missing": missing,
    }
    write_json(payload)
    sys.exit(1)
else:
    # machine-readable
    payload = {
        "status": "ok",
        "target": display_name,
        "sym_file": args.sym,
        "missing": [],
    }
    write_json(payload)
    sys.exit(0)
