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
from buildscripts.client.jiraclient import JiraAuth, JiraClient
from buildscripts.util.codeowners_utils import Owners

JIRA_SERVER = "https://jira.mongodb.org"

CC_LIB_SUFFIX = "_with_debug"


def move_header(
    fix_target: str, header: str, new_dep: Optional[str] = None, add_header: bool = False
) -> None:
    bd_utils.bd_remove([fix_target], "hdrs", [header])
    if new_dep:
        bd_utils.bd_add([fix_target], "deps", [new_dep])
        # if add_header:
        #     bd_utils.bd_add([new_dep], "hdrs", [header])


def undo_header_move(
    fix_target: str, header: str, new_dep: Optional[str] = None, remove_header: bool = False
) -> None:
    if new_dep:
        bd_utils.bd_remove([fix_target], "deps", [new_dep])
        # if remove_header:
        #     bd_utils.bd_remove([new_dep], "hdrs", [header])
    # bd_utils.bd_add([fix_target], "hdrs", [header])


def new_filegroup(header: str, issue_key: str) -> str:
    package = header.split(":")[0] + ":__pkg__"
    target_name = header.split(".")[0]
    fg_name = target_name.split(":")[1] + "_hdrs"
    fg_label = package.split(":")[0] + f":{fg_name}"
    bd_utils.bd_new(package, "filegroup", fg_name)
    bd_utils.bd_add([fg_label], "srcs", [header])
    return fg_label


def fix_cycle(fix_target: str, header: str, issue_key: str) -> str:
    fg_label = new_filegroup(header, issue_key)
    bd_utils.bd_add([fix_target], "hdrs", [fg_label])
    return fg_label


def todo_comment(issue_key: str, header: str, new_dep: str, fg_label: str) -> None:
    comment = f"TODO({issue_key}): Remove cycle created by moving {header} to {new_dep}".replace(
        " ", "\ "
    )
    bd_utils.bd_comment([fg_label], comment)


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
        self.bazel_exec = "bazel"
        auth = JiraAuth()
        auth.pat = os.environ["JIRA_TOKEN"]
        self.jira_client = JiraClient(JIRA_SERVER, auth, dry_run=False)
        self.owners = Owners()
        self.team_issues = {}

    def _query(
        self, query: str, config: bool = False, args: List[str] = []
    ) -> subprocess.CompletedProcess:
        query_cmd = "cquery"

        p = subprocess.run(
            [self.bazel_exec, query_cmd] + args + [query],
            capture_output=True,
            text=True,
            check=True,
        )
        return p

    def _build(self, target: str) -> subprocess.CompletedProcess:
        p = subprocess.run(
            [self.bazel_exec, "build"] + [target],
            capture_output=True,
            text=True,
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

            # ignore if cpp file of respective header is a src of our fix target
            if new_dep == target:
                continue

            buildozer_cmds = [f"buildozer 'remove hdrs {hdr}' {target}"]
            if not has_header:
                buildozer_cmds += [f"buildozer 'add hdrs {hdr}' {new_dep}"]
            if self._check_dep_exists(target, new_dep):
                print(f"Dep {new_dep} is already a dependency")
                new_dep = None
            else:
                buildozer_cmds += [f"buildozer 'add deps {new_dep}' {target}"]
            move_header(target, hdr, new_dep, has_header)
            p = self._build(target)
            if p.returncode == 0:
                target_fixes["fixes"][hdr] = buildozer_cmds
            elif p.returncode == 1 and "cycle in dependency graph" in p.stderr:
                target_fixes["cycles"][hdr] = buildozer_cmds
                issue_key = self._create_jira_ticket(hdr)
                fg_label = fix_cycle(target, hdr, issue_key)
                todo_comment(issue_key, hdr, new_dep, fg_label)
                undo_header_move(target, hdr, new_dep, has_header)
            else:
                print("Unexpected bazel failure.")

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

    def _create_jira_ticket(self, header: str) -> str:
        summary = "Fix cycle created by " + header
        header_file_path = header.replace(":", "/")[2:]
        assigned_teams = self.owners.get_jira_team_owner(header_file_path)
        if not assigned_teams:
            assigned_teams = ["Build"]
        teams_key = ",".join(sorted(assigned_teams))
        if teams_key in self.team_issues:
            description = header
            issue = self.team_issues[teams_key]
            # Add new header to description
            issue.update(description=(issue.fields.description or "") + "\n" + description)
        else:
            description = (
                "[Header relocation info|https://github.com/10gen/mongo/blob/master/bazel/docs/header_cycle_resolution.md]\nPlease resolve dependency issues with the following headers:\n"
                + header
            )
            issue = self.jira_client.create_issue(
                issue_type="Bug",
                summary=summary,
                description=description,
                assigned_teams=assigned_teams,
                jira_project="SERVER",
            )
            self.team_issues[teams_key] = issue
        if not issue:
            return ""
        return issue.key

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
