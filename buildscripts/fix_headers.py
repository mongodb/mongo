"""Cleanup Bazel target headers
1. Evaluate expression into a list of cc_library targets.
2. Identify headers defined outside of the package directory.
3. Lookup target that should claim a given header.
4. If said target exists, check for cycles by modifying BUILD.bazel and building.
5. Print report with targets and buildozer commands to fix each one.
"""

# TODO(SERVER-94780) Add buildozer dep to poetry
import json
import os
import pprint
import subprocess
import sys
from typing import Annotated, Dict, List, Optional, Tuple

import typer

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import buildscripts.util.buildozer_utils as bd_utils

CC_LIB_SUFFIX = "_with_debug"


def move_header(
    fix_target: str, header: str, new_dep: Optional[str] = None, add_header: bool = False
) -> None:
    bd_utils.bd_remove([fix_target], "hdrs", [header])
    if new_dep:
        bd_utils.bd_add([fix_target], "deps", [new_dep])
        if add_header:
            bd_utils.bd_add([new_dep], "hdrs", [header])


def undo_header_move(
    fix_target: str, header: str, new_dep: Optional[str] = None, remove_header: bool = False
) -> None:
    if new_dep:
        bd_utils.bd_remove([fix_target], "deps", [new_dep])
        if remove_header:
            bd_utils.bd_remove([new_dep], "hdrs", [header])
    bd_utils.bd_add([fix_target], "hdrs", [header])


def useful_print(fixes: Dict) -> None:
    for target, target_fixes in fixes.items():
        print("-", target)
        print("  Fixes:\n")
        for header, commands in target_fixes["fixes"].items():
            print(f"    -{header}:")
            for cmd in commands:
                print("      ", cmd)


class HeaderFixer:
    def __init__(self):
        # TODO(SERVER-94781) Remove SCons dep
        subprocess.run(
            [
                sys.executable,
                "buildscripts/scons.py",
                "--build-profile=opt",
                "--bazel-includes-info=dummy",  # TODO Allow no library to be passed.
                "--libdeps-linting=off",
                "--ninja=disabled",
                "$BUILD_ROOT/scons/$VARIANT_DIR/sconf_temp",
            ]
        )
        with open(".bazel_include_info.json") as f:
            bazel_include_info = json.load(f)
        self.bazel_exec = bazel_include_info["bazel_exec"]
        self.bazel_config = bazel_include_info["config"]

    def _query(
        self, query: str, config: bool = False, args: List[str] = []
    ) -> subprocess.CompletedProcess:
        query_cmd = "cquery" if config else "query"
        p = subprocess.run(
            [self.bazel_exec, query_cmd] + self.bazel_config + args + [query],
            capture_output=True,
            text=True,
            check=True,
        )
        return p

    def _build(self, target: str) -> subprocess.CompletedProcess:
        p = subprocess.run(
            [self.bazel_exec, "build"] + self.bazel_config + [target],
            capture_output=True,
            text=True,
            check=True,
        )
        return p

    def _fix_package(self, package: str):
        pass

    def _create_header_target(self):
        pass

    def _find_misplaced_headers(self, target: str) -> List[str]:
        p = self._query(f"labels(hdrs,{target}{CC_LIB_SUFFIX})")
        misplaced_headers = []
        target_package = target.split(":")[0] + ":"
        for line in p.stdout.splitlines():
            if not line.startswith("//"):
                continue
            if "." not in line:
                continue
            # skip if local
            if line.startswith(target_package):
                continue
            misplaced_headers.append(line.split(" ")[0])

        return misplaced_headers

    def _find_header_target(self, header: str) -> Tuple[Optional[str], bool]:
        potential_target = header.split(".")[0]
        p = self._query(f"attr(srcs,{potential_target}.cpp,//...)")
        target = None
        for line in p.stdout.splitlines():
            line = line.split()[0]
            if not line.startswith("//"):
                continue
            if line.endswith(CC_LIB_SUFFIX):
                target = line[: -len(CC_LIB_SUFFIX)]

        if not target:
            return None, False

        p = self._query(f"filter('{header}',labels(hdrs,{target}{CC_LIB_SUFFIX}))")
        filter_res = [line for line in p.stdout.splitlines() if line.startswith("//")]
        if filter_res == []:
            return target, False
        return target, True

    def _get_build_file(self, target: str) -> Optional[str]:
        p = self._query(f"buildfiles({target})")
        for line in p.stdout.splitlines():
            if line.startswith("//src"):
                return line.strip()

        return None

    def _check_dep_exists(self, fix_target: str, dep: str) -> bool:
        p = self._query(f"filter({dep}$,deps({fix_target}))")
        for line in p.stdout.splitlines():
            if line.startswith("//") and line.split()[0] == dep:
                return True

        return False

    def _fix_target(self, target: str) -> Dict:
        target_fixes = {"fixes": {}, "cycles": {}}
        orphaned_headers = []
        for hdr in self._find_misplaced_headers(target):
            new_dep, has_header = self._find_header_target(hdr)
            if not new_dep:
                orphaned_headers.append(hdr)
                continue

            buildozer_cmds = [f"buildozer 'remove hdrs {hdr}' {target}"]
            if not has_header:
                buildozer_cmds += [f"buildozer 'add hdrs {hdr}' {new_dep}"]
            if self._check_dep_exists(target, new_dep):
                new_dep = None
            else:
                buildozer_cmds += [f"buildozer 'add deps {new_dep}' {target}"]
            move_header(target, hdr, new_dep, has_header)
            p = self._build(target)
            if p.returncode == 0:
                target_fixes["fixes"][hdr] = buildozer_cmds
            elif p.returncode == 1 and "cycle in dependency graph" in p.stdout:
                target_fixes["cycles"][hdr] = buildozer_cmds
            else:
                print("Unexpected bazel failure.")
            undo_header_move(target, hdr, new_dep, has_header)

        print(f"Orphaned headers for {target}")
        print("\n".join(orphaned_headers))
        return target_fixes

    def _evaluate_target_expression(self, target_exp: str) -> List[str]:
        p = self._query(
            f"filter('.*{CC_LIB_SUFFIX}$',kind(cc_library,deps({target_exp}, 1)))",
            ["--noimplicit_deps"],
        )
        return [
            line.split()[0][: -len(CC_LIB_SUFFIX)]
            for line in p.stdout.splitlines()
            if line.startswith("//")
        ]

    def fix_targets(self, target_exp: str) -> Dict:
        fixes = {}
        for target in self._evaluate_target_expression(target_exp):
            fixes[target] = self._fix_target(target)

        return fixes


def main(
    target_exp: Annotated[str, typer.Argument()],
    output_file: Annotated[str, typer.Option()] = "",
    copy_format: Annotated[bool, typer.Option()] = False,
):
    hf = HeaderFixer()
    fixes = hf.fix_targets(target_exp)
    json_output = pprint.pformat(json.dumps(fixes), compact=False).replace("'", '"')
    if output_file:
        with open(output_file, "w") as f:
            print(json_output, filename, file=f)
    elif copy_format:
        useful_print(fixes)
    else:
        print(json_output)


if __name__ == "__main__":
    typer.run(main)
