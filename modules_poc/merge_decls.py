#!/usr/bin/env python3
import json
import multiprocessing
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed, wait
from datetime import datetime
from typing import Any, TypedDict

import pyzstd
import typer  # nicer error dump on exceptions
from progressbar import ProgressBar, progressbar


class Decl(TypedDict):
    display_name: str
    kind: str
    loc: str
    mod: str
    other_mods: dict[str, set[str]]  # merged
    used_from: dict[str, set[str]]  # merged
    usr: str
    visibility: str
    defined: bool


all_decls: dict[str, Decl] = {}


def merge_decls(decls: list[Decl]):
    for decl in decls:
        decls = []  # hide from traceback
        merge_decl(decl)


def merge_decl(decl: Decl):
    other_mods = decl.get("other_mods", {})
    used_from = decl["used_from"]
    usr = decl["usr"]
    if usr not in all_decls:
        # First time seeing this decl - no merging needed
        for mod in used_from:
            if type(used_from[mod]) != set:
                used_from[mod] = set(used_from[mod])
        all_decls[usr] = decl
        return

    old = all_decls[usr]

    # Merge used_from into old_used_from
    old_used_from = old["used_from"]
    for mod, locs in used_from.items():
        if not mod:
            mod = "__NONE__"
        old_used_from.setdefault(mod, set()).update(locs)

    old_other_mods = old.get("other_mods", {})

    # Merge other_mods into old_other_mods
    for other, val in other_mods.items():
        if isinstance(val, set):
            old_other_mods.setdefault(other, set()).update(val)
        else:
            old_other_mods.setdefault(other, set()).add(val)

    mod = decl["mod"]
    replace = decl["defined"] and not old["defined"]
    if replace:
        # Make this the primary decl, even if from same mod
        all_decls[usr] = decl
        decl["used_from"] = old_used_from
        if decl["loc"] != old["loc"]:
            old_other_mods.setdefault(old["mod"], set()).add(old["loc"])
            if mod in old_other_mods and decl["loc"] in old_other_mods[mod]:
                old_other_mods[mod].remove(decl["loc"])
                if not old_other_mods[mod]:
                    del old_other_mods[mod]
        if old_other_mods:
            decl["other_mods"] = old_other_mods
    else:
        if decl["loc"] != old["loc"]:
            old_other_mods.setdefault(mod, set()).add(decl["loc"])
        if old_other_mods:
            old["other_mods"] = old_other_mods

    # assert decl["loc"] == old["loc"]
    assert (
        decl["kind"] == old["kind"]
        or (decl["kind"] == "CLASS_DECL" and old["kind"] == "CLASS_TEMPLATE")
        or (decl["kind"] == "CLASS_TEMPLATE" and old["kind"] == "CLASS_DECL")
    )
    # assert decl["display_name"] == old["display_name"]  # TODO ugh sometimes mongo:: screws it up


class Timer:
    def __init__(self):
        self.start = datetime.now()

    def mark(self, label: str):
        if 1:
            elapsed = datetime.now() - self.start
            print(f"{label}: {elapsed}")


def worker(paths: list[bytes]):
    # for path in paths:
    while True:
        try:
            path = paths.pop()
        except IndexError:
            return list(all_decls.values())

        with pyzstd.ZstdFile(path, read_size=2 * 1024 * 1024) as f:
            merge_decls(json.loads(f.read()))


def is_submodule_usage(decl: Decl, mod: str) -> bool:
    return decl["mod"] == mod or mod.startswith(decl["mod"] + ".")


def get_paths(timer: Timer):
    project_root = os.path.dirname(os.path.abspath(sys.argv[0])) + "/.."
    subprocess.run(
        ["bazel", "build", "--config=mod-scanner", "//src/mongo/..."], cwd=project_root, check=True
    )
    timer.mark("built")

    proc = subprocess.run(
        [
            "bazel",
            "aquery",
            "--config=mod-scanner",
            'outputs(".*mod_scanner_decls.json.*", mnemonic(ModScanner, //src/mongo/...))',
            "--noinclude_commandline",
            "--noinclude_artifacts",
        ],
        capture_output=True,
        text=True,
        cwd=project_root,
        check=True,
    )

    outputs = []
    for line in proc.stdout.split("\n"):
        if line.startswith("  Environment:") and "MOD_SCANNER_OUTPUT=" in line:
            m = re.search("MOD_SCANNER_OUTPUT=([^,]+),", line)
            if m:
                outputs.append(m.group(1))
    timer.mark("sources_found")
    return outputs


def main(
    jobs: int = typer.Option(os.cpu_count(), "--jobs", "-j"),
    intra_module: bool = typer.Option(False, help="Include intra-module accesses"),
):
    timer = Timer()
    paths = get_paths(timer)
    num_paths = len(paths)

    if jobs > 1:
        with multiprocessing.Manager() as manager:
            with ProcessPoolExecutor(jobs) as pool:
                workers = set()
                shared_paths = manager.list(paths)
                for _ in range(jobs):
                    workers.add(pool.submit(worker, shared_paths))  # type:ignore

                with ProgressBar(max_value=num_paths, prefix="processing inputs: ") as bar:
                    while True:
                        done, _ = wait(workers, timeout=0.1, return_when="FIRST_EXCEPTION")
                        for d in done:
                            if d.exception():
                                raise d.exception()

                        remaining_files = len(shared_paths) + jobs - len(done)
                        bar.update(num_paths - remaining_files)
                        if remaining_files == 0:
                            break

                timer.mark("all paths consumed")
                for result in as_completed(workers):
                    merge_decls(result.result())
                timer.mark("merged results")

    else:
        for path in progressbar(paths):
            with pyzstd.ZstdFile(path, read_size=2 * 1024 * 1024) as f:
                merge_decls(json.loads(f.read()))
        timer.mark("processed input")

    out: Any = [dict(d) for d in all_decls.values()]  # shallow copy each decl
    if not intra_module:
        for decl in out:
            decl["used_from"] = {
                mod: locs
                for mod, locs in decl["used_from"].items()
                if not is_submodule_usage(decl, mod)
            }
        out = [d for d in out if d["used_from"]]

    for decl in out:
        # go from {$MOD: $LOCS} map to [{mod: $MOD, locs: $LOCS}] list of
        # objects which is easier to work with in mongo aggregations
        decl["used_from"] = [{"mod": k, "locs": sorted(v)} for k, v in decl["used_from"].items()]  # type: ignore
        if "other_mods" in decl:
            decl["other_mods"] = {k: sorted(v) for k, v in decl["other_mods"].items()}  # type: ignore
    timer.mark("massaged output for json")

    with open("merged_decls.json", "w") as f:
        json.dump(out, f)
    timer.mark("dumped json")

    found_violations = False
    for decl in sorted(all_decls.values(), key=lambda d: d["display_name"]):
        violations = []
        match decl["visibility"]:
            case "private":
                err = f"Illegal use of {decl['display_name']} outside of module {decl['mod']}:"
                for mod, locs in decl["used_from"].items():
                    if not is_submodule_usage(decl, mod):
                        for loc in locs:
                            violations.append(f"    {loc} ({mod})")

            case "file_private":
                err = f"Illegal use of {decl['display_name']} outside of its file family:"

                # file_base is the portion of the file name that defines the family
                # e.g. bazel-out/blah/src/mongo/db/foo_details.h -> src/mongo/db/foo
                file_base = decl["loc"].split(".")[0]
                if index := file_base.index("src/mongo/"):
                    file_base = file_base[index:]
                file_base = re.sub(r"_(internal|detail)s?$", "", file_base)
                assert file_base.startswith("src/mongo/")

                file_family_regex = re.compile(
                    rf"(?:.*/)?{file_base}(?:_(?:internals?|details?|test|bm|mock)(_.*)?)?\."
                )
                assert file_family_regex.match(decl["loc"])  # sanity check

                for mod, locs in decl["used_from"].items():
                    for loc in locs:
                        # Must be in the same module even if file family matches.
                        # This helps prevent accidental matches.
                        if mod != decl["mod"] or not file_family_regex.match(loc):
                            violations.append(f"    {loc} ({mod})")
            case _:  # ignore other visibility types
                continue

        if violations:
            found_violations = True
            print(err)
            print(f"  loc: {decl['loc']}")
            print("  usages:")
            print("\n".join(violations))
    timer.mark("checked for privacy violations")

    sys.exit(found_violations)  # bools are ints, so False(0) is success and True(1) is failure


if __name__ == "__main__":
    typer.run(main)
