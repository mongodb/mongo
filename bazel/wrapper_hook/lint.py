import argparse
import os
import pathlib
import platform
import subprocess
import sys
import tempfile
from typing import List

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

LARGE_FILE_THRESHOLD = 10 * 1024 * 1024 #10MiB

SUPPORTED_EXTENSIONS = (".cpp", ".c", ".h", ".hpp", ".py", ".js", ".mjs", ".json", ".lock", ".toml", ".defs", ".inl", ".idl")


class LinterFail(Exception):
    pass

def create_build_files_in_new_js_dirs() -> None:
    base_dirs = ["src/mongo/db/modules/enterprise/jstests", "jstests"]
    for base_dir in base_dirs:
        for root, dirs, _ in os.walk(base_dir):
            for dir in dirs:
                full_dir = os.path.join(root, dir)
                build_file_path = os.path.join(full_dir, "BUILD.bazel")
                if not os.path.isfile(build_file_path):
                    js_files = [f for f in os.listdir(full_dir) if f.endswith(".js")]
                    if js_files:
                        with open(build_file_path, "w", encoding="utf-8") as build_file:
                            build_file.write("""load("//bazel:mongo_js_rules.bzl", "mongo_js_library", "all_subpackage_javascript_files")

package(default_visibility = ["//visibility:public"])

mongo_js_library(
    name = "all_javascript_files",
    srcs = glob([
        "*.js",
    ]),
)

all_subpackage_javascript_files()
""")
                        print(f"Created BUILD.bazel in {full_dir}")


def list_files_with_targets(bazel_bin: str) -> List:
    return [
        line.strip()
        for line in subprocess.run(
            [bazel_bin, "query", 'kind("source file", deps(//...))', "--keep_going"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.splitlines()
    ]

class LintRunner:
    def __init__(self, keep_going: bool, bazel_bin: str):
        self.keep_going = keep_going
        self.bazel_bin = bazel_bin
        self.fail = False

    def list_files_without_targets(
        self,
        files_with_targets: List[str],
        type_name: str,
        ext: str,
        dirs: List[str],
    ) -> bool:
        # rules_lint only checks files that are in targets, verify that all files in the source tree
        # are contained within targets.

        exempt_list = {
            # TODO(SERVER-101360): Remove the exemptions below once resolved.
            "src/mongo/crypto/fle_options.cpp",
            # TODO(SERVER-101368): Remove the exemptions below once resolved.
            "src/mongo/db/modules/enterprise/src/streams/commands/update_connection.cpp",
            # TODO(SERVER-101370): Remove the exemptions below once resolved.
            "src/mongo/db/modules/enterprise/src/streams/third_party/mongocxx/dist/mongocxx/test_util/client_helpers.cpp",
            # TODO(SERVER-101371): Remove the exemptions below once resolved.
            "src/mongo/db/modules/enterprise/src/streams/util/tests/concurrent_memory_aggregator_test.cpp",
            # TODO(SERVER-101375): Remove the exemptions below once resolved.
            "src/mongo/platform/decimal128_dummy.cpp",
            # TODO(SERVER-101377): Remove the exemptions below once resolved.
            "src/mongo/util/icu_init_stub.cpp",
            # TODO(SERVER-101377): Remove the exemptions below once resolved.
            "src/mongo/util/processinfo_emscripten.cpp",
            "src/mongo/util/processinfo_macOS.cpp",
            "src/mongo/util/processinfo_solaris.cpp",
        }

        typed_files_in_targets = [line for line in files_with_targets if line.endswith(f".{ext}")]

        print(f"Checking that all {type_name} files have BUILD.bazel targets...")

        all_typed_files = (
            subprocess.check_output(
                ["find", *dirs, "-name", f"*.{ext}"],
                stderr=subprocess.STDOUT,
            )
            .decode("utf-8")
            .splitlines()
        )

        # Convert typed_files_in_targets to a set for easy comparison
        typed_files_in_targets_set = set()
        for file in typed_files_in_targets:
            # Remove the leading "//" and replace ":" with "/"
            clean_file = file.lstrip("//").replace(":", "/")
            typed_files_in_targets_set.add(clean_file)

        # Create a new list of files that are in all_typed_files but not in typed_files_in_targets
        new_list = []
        for file in all_typed_files:
            if file not in typed_files_in_targets_set and file not in exempt_list:
                if "bazel_rules_mongo" in file:
                    # Skip files in bazel_rules_mongo, since it has its own Bazel repo
                    continue

                new_list.append(file)

        if len(new_list) != 0:
            print(f"Found {type_name} files without BUILD.bazel definitions:")
            for file in new_list:
                print(f"\t{file}")
            print("")
            print(
                f"Please add these to a {ext}_library target in a BUILD.bazel file in their directory"
            )
            print("Run the following to attempt to fix the issue automatically:")
            print("\tbazel run lint --fix")
            self.fail = True
            if not self.keep_going:
                raise LinterFail("File missing bazel target.")

        print(f"All {type_name} files have BUILD.bazel targets!")

    def run_bazel(self, target: str, args: List = []):
        p = subprocess.run([self.bazel_bin, "run", target] + (["--"] + args if args else []))
        if p.returncode != 0:
            self.fail = True
            if not self.keep_going:
                raise LinterFail("Linter failed")

    def simple_file_size_check(self, files_to_lint: List[str]):
        for file in files_to_lint:
            if os.path.getsize(file) > LARGE_FILE_THRESHOLD:
                print(f"File {file} exceeds large file threshold of {LARGE_FILE_THRESHOLD}")
                self.fail = True
                if not self.keep_going:
                    raise LinterFail("File too large")


def _git_distance(args: list) -> int:
    command = ["git", "rev-list", "--count"] + args
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running git command: {' '.join(command)}")
        print(f"stderr: {e.stderr.strip()}")
        print(f"stdout: {e.stdout.strip()}")
        raise
    return int(result.stdout.strip())


def _get_merge_base(args: list) -> str:
    command = ["git", "merge-base"] + args
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return result.stdout.strip()


def _git_diff(args: list) -> str:
    command = ["git", "diff"] + args
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return result.stdout.strip() + os.linesep


def _git_unstaged_files() -> str:
    command = ["git", "ls-files", "--others", "--exclude-standard"]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return result.stdout.strip() + os.linesep


def _get_files_changed_since_fork_point(origin_branch: str = "origin/master") -> List[str]:
    """Query git to get a list of files in the repo from a diff."""
    # There are 3 diffs we run:
    # 1. List of commits between origin/master and HEAD of current branch
    # 2. Cached/Staged files (--cached)
    # 3. Working Tree files git tracks

    fork_point = _get_merge_base(["HEAD", origin_branch])

    diff_files = _git_diff(["--name-only", f"{fork_point}..HEAD"])
    diff_files += _git_diff(["--name-only", "--cached"])
    diff_files += _git_diff(["--name-only"])
    diff_files += _git_unstaged_files()

    file_set = {
        os.path.normpath(os.path.join(os.curdir, line.rstrip()))
        for line in diff_files.splitlines()
        if line
    }

    return list(file_set)

def get_parsed_args(args):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--lint-yaml-project",
        type=str,
        default="mongodb-mongo-master",
        required=False,
        help="Run evergreen yaml linter for specified project",
    )
    parser.add_argument(
        "--fix",
        action="store_true",
        default=False,
        help="Apply linter fixes",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        default=False,
        help="Run linter on all targets",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=False,
    )
    parser.add_argument(
        "--fail-on-validation",
        action="store_true",
        default=False,
    )
    parser.add_argument(
        "--origin-branch",
        type=str,
        default="origin/master",
        help="Base branch to compare changes against",
    )
    parser.add_argument(
        "--large-files",
        action="store_true",
        default=False
    )
    parser.add_argument(
        "--keep-going",
        action="store_true",
        default=False,
        help="Keep going after failures",
    )
    return parser.parse_known_args(args)

def lint_mod(lint_runner: LintRunner):
    lint_runner.run_bazel("//modules_poc:mod_mapping", ["--validate-modules"])
    #TODO add support for the following steps
    #subprocess.run([bazel_bin, "run", "//modules_poc:merge_decls"], check=True)
    #subprocess.run([bazel_bin, "run", "//modules_poc:browse", "--", "merged_decls.json", "--parse-only"], check=True)

def run_rules_lint(bazel_bin: str, args: List[str]):
    parsed_args, args = get_parsed_args(args)
    if platform.system() == "Windows":
        print("eslint not supported on windows")
        raise LinterFail("Unsupported platform")

    if parsed_args.fix:
        create_build_files_in_new_js_dirs()

    keep_going = parsed_args.keep_going
    lr = LintRunner(keep_going, bazel_bin)

    files_with_targets = list_files_with_targets(bazel_bin)
    lr.list_files_without_targets(files_with_targets, "C++", "cpp", ["src/mongo"])
    lr.list_files_without_targets(
        files_with_targets, "javascript", "js", ["src/mongo", "jstests"],
    )
    lr.list_files_without_targets(
        files_with_targets, "python", "py", ["src/mongo", "buildscripts", "evergreen"],
    )
    lint_all = parsed_args.all or "..." in args or "//..." in args
    files_to_lint = [arg for arg in args if not arg.startswith("-")]
    if not lint_all and files_to_lint:
        origin_branch = parsed_args.origin_branch
        max_distance = 100
        distance = _git_distance([f"{origin_branch}..HEAD"])
        if distance > max_distance:
            print(
                f"The number of commits between current branch and origin branch ({origin_branch}) is too large: {distance} commits (> {max_distance} commits)."
            )
            print(
                "Please update your local branch with the latest changes from origin, or use `bazel run lint -- --origin-branch=other_branch` to select a different origin branch"
            )
            lint_all = True
        else:
            files_to_lint = [
                file
                for file in _get_files_changed_since_fork_point(origin_branch)
                if file.endswith((SUPPORTED_EXTENSIONS))
            ]

    if lint_all or "sbom.json" in files_to_lint:
        lr.run_bazel("//buildscripts:sbom_linter")

    if lint_all or any(file.endswith((".h", ".cpp")) for file in files_to_lint):
        lr.run_bazel("//buildscripts:quickmongolint", ["lint"])

    if lint_all or any(
        file.endswith((".cpp", ".c", ".h", ".py", ".idl"))
        for file in files_to_lint
    ):
        lr.run_bazel("//buildscripts:errorcodes", ["--quiet"])

    if lint_all:
        lr.run_bazel("//buildscripts:pyrightlint", ["lint-all"])
    elif any(file.endswith(".py") for file in files_to_lint):
        lr.run_bazel("//buildscripts:pyrightlint", ["lints"] + files_to_lint)

    if lint_all or "poetry.lock" in files_to_lint or "pyproject.toml" in files_to_lint:
        lr.run_bazel("//buildscripts:poetry_lock_check")

    if lint_all or any(file.endswith(".yml") for file in files_to_lint):
        lr.run_bazel("buildscripts:validate_evg_project_config", [f"--evg-project-name={parsed_args.lint_yaml_project}", "--evg-auth-config=.evergreen.yml"])

    if lint_all or parsed_args.large_files:
        lr.run_bazel("buildscripts:large_file_check", ["--exclude", "src/third_party/*"])
    else:
        lr.simple_file_size_check(files_to_lint)



    if lint_all or any(
        file.endswith((".cpp", ".c", ".h", ".hpp", ".idl", ".inl", ".defs"))
        for file in files_to_lint
    ):
        lint_mod(lr)

    if lr.fail:
        raise LinterFail("Linter(s) failed")

    # Default to linting everything in rules_lint if no path was passed in.
    if len([arg for arg in args if not arg.startswith("--")]) == 0:
        args = ["//..."] + args

    fix = ""
    with tempfile.NamedTemporaryFile(delete=False) as buildevents:
        buildevents_path = buildevents.name

    for linter in ["eslint", "ruff"]:
        args.append(f"--aspects=//tools/lint:linters.bzl%{linter}")

    args.extend(
        [
            # Allow lints of code that fails some validation action
            # See https://github.com/aspect-build/rules_ts/pull/574#issuecomment-2073632879
            "--norun_validations",
            f"--build_event_json_file={buildevents_path}",
            "--output_groups=rules_lint_human",
            "--remote_download_regex='.*AspectRulesLint.*'",
        ]
    )

    # This is a rudimentary flag parser.
    if parsed_args.fail_on_validation:
        args.extend(["--@aspect_rules_lint//lint:fail_on_violation", "--keep_going"])

    # Allow a `--fix` option on the command-line.
    # This happens to make output of the linter such as ruff's
    # [*] 1 fixable with the `--fix` option.
    # so that the naive thing of pasting that flag to lint.sh will do what the user expects.
    if parsed_args.fix:
        fix = "patch"
        args.extend(["--@aspect_rules_lint//lint:fix", "--output_groups=rules_lint_patch"])

    # the --dry-run flag must immediately follow the --fix flag
    if parsed_args.dry_run:
        fix = "print"

    args = (
        [arg for arg in args if arg.startswith("--") and arg != "--"]
        + ["--"]
        + [arg for arg in args if not arg.startswith("--")]
    )

    # Actually run the lint itself
    subprocess.run([bazel_bin, "build"] + args, check=True)

    # Parse out the reports from the build events
    filter_expr = '.namedSetOfFiles | values | .files[] | select(.name | endswith($ext)) | ((.pathPrefix | join("/")) + "/" + .name)'

    # Maybe this could be hermetic with bazel run @aspect_bazel_lib//tools:jq or sth
    # jq on windows outputs CRLF which breaks this script. https://github.com/jqlang/jq/issues/92
    valid_reports = (
        subprocess.run(
            ["jq", "--arg", "ext", ".out", "--raw-output", filter_expr, buildevents_path],
            capture_output=True,
            text=True,
            check=True,
        )
        .stdout.strip()
        .split("\n")
    )

    failing_reports = 0
    for report in valid_reports:
        # Exclude coverage reports, and check if the output is empty.
        if "coverage.dat" in report or not os.path.exists(report) or not os.path.getsize(report):
            # Report is empty. No linting errors.
            continue
        with open(report, "r", encoding="utf-8") as f:
            file_contents = f.read().strip()
            if file_contents == "All checks passed!":
                # Report is successful. No linting errors.
                continue

            print(f"From {report}:")
            print(file_contents)
            print()
            failing_reports += 1

    # Apply fixes if requested
    if fix:
        valid_patches = (
            subprocess.run(
                ["jq", "--arg", "ext", ".patch", "--raw-output", filter_expr, buildevents_path],
                capture_output=True,
                text=True,
                check=True,
            )
            .stdout.strip()
            .split("\n")
        )

        for patch in valid_patches:
            # Exclude coverage, and check if the patch is empty.
            if "coverage.dat" in patch or not os.path.exists(patch) or not os.path.getsize(patch):
                # Patch is empty. No linting errors.
                continue

            if fix == "print":
                print(f"From {patch}:")
                with open(patch, "r", encoding="utf-8") as f:
                    print(f.read())
                print()
            elif fix == "patch":
                subprocess.run(
                    ["patch", "-p1"], check=True, stdin=open(patch, "r", encoding="utf-8")
                )
            else:
                print(f"ERROR: unknown fix type {fix}", file=sys.stderr)
                raise LinterFail("Unknown fix type")
    elif failing_reports != 0:
        raise LinterFail("Failing reports")
