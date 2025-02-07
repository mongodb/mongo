#!/usr/bin/env python3
import glob
import json
import multiprocessing
import os
from concurrent.futures import ProcessPoolExecutor, as_completed, wait
from datetime import datetime
from typing import Any, TypedDict

import typer  # nicer error dump on exceptions
import yaml
from progressbar import ProgressBar, progressbar

try:
    from yaml import CDumper as Dumper
    from yaml import CLoader as Loader  # noqa: F401 Not used right now but may be again
except ImportError:
    raise RuntimeError("Why no cYaml?")
    # from yaml import Loader, Dumper


class Decl(TypedDict):
    display_name: str
    kind: str
    loc: str
    mod: str
    other_mods: dict[str, set[str]]  # merged
    used_from: dict[str, set[str]]  # merged
    usr: str
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
        if mod != old["mod"]:
            if mod in old_other_mods:
                del old_other_mods[mod]  # we are not an "other"
            old_other_mods.setdefault(old["mod"], set()).add(old["loc"])
        if old_other_mods:
            decl["other_mods"] = old_other_mods
    else:
        if mod != old["mod"]:
            old_other_mods.setdefault(mod, set()).add(decl["loc"])
        if old_other_mods:
            old["other_mods"] = old_other_mods

    # assert decl["loc"] == old["loc"]
    assert (
        decl["kind"] == old["kind"]
        # These are weird special cases where it sometimes ends up on
        # CLASS_DECL rather than the CLASS_TEMPLATE. Not sure why?
        or decl["display_name"].startswith("StackBufBuilderBase")
        or decl["display_name"].startswith("Sorter")
        or decl["display_name"].startswith("SortIteratorInterface")
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

        with open(path) as f:
            merge_decls(json.loads(f.read()))


def main(
    jobs: int = typer.Option(os.cpu_count(), "--jobs", "-j"),
    intra_module: bool = typer.Option(False, help="Include intra-module accesses"),
    generate_yaml: bool = False,
):
    timer = Timer()
    paths = glob.glob(b"bazel-bin/**/*.mod_scanner_decls.json", recursive=True)
    num_paths = len(paths)
    timer.mark("globbed")

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
            with open(path) as f:
                merge_decls(json.loads(f.read()))
        timer.mark("processed input")

    if not intra_module:
        for decl in all_decls.values():
            if decl["mod"] in decl["used_from"]:
                del decl["used_from"][decl["mod"]]

    out: Any = [d for d in all_decls.values() if d["used_from"]]
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

    if generate_yaml:
        for decl in out:
            decl["used_from"] = {u["mod"]: u["locs"] for u in decl["used_from"]}  # type: ignore
        # sort by file to make it easier to use
        out.sort(key=lambda d: d["loc"])

        timer.mark("massaged output for yaml")

        with open("merged_decls.yaml", "w") as f:
            yaml.dump(out, f, Dumper=Dumper, width=1000000)
        timer.mark("dumped yaml")

    out = list(
        {k: v for k, v in d.items() if not k == "used_from"}
        for d in all_decls.values()
        if d["mod"] == "__NONE__"
        # These are parts of other things (classes and enums) that should already be included.
        and d["kind"] not in ("CXX_METHOD", "CONSTRUCTOR", "ENUM_CONSTANT_DECL", "FIELD_DECL")
    )
    out.sort(key=lambda d: d["display_name"])
    timer.mark("massaged output for unowned.yaml")

    with open("unowned.yaml", "w") as f:
        yaml.dump(out, f, Dumper=Dumper, width=1000000)
    timer.mark("dumped unowned.yaml")


if __name__ == "__main__":
    typer.run(main)
