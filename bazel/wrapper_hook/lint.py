import argparse
import difflib
import os
import pathlib
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Optional

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

LARGE_FILE_THRESHOLD = 10 * 1024 * 1024  # 10MiB
LINT_FAILURE_DETAIL_ENV_VAR = "MONGO_BAZEL_LINT_FAILURE_FILE"


def _read_optional_bytes(path: pathlib.Path) -> bytes | None:
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


def _restore_optional_bytes(path: pathlib.Path, data: bytes | None) -> None:
    if data is None:
        path.unlink(missing_ok=True)
        return

    path.write_bytes(data)


def _display_path(path: pathlib.Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def _get_unified_diff(path: pathlib.Path, before: bytes | None, after: bytes | None) -> str:
    display_path = _display_path(path).lstrip("/\\")
    before_lines = (before or b"").decode("utf-8", errors="replace").splitlines(keepends=True)
    after_lines = (after or b"").decode("utf-8", errors="replace").splitlines(keepends=True)
    diff = difflib.unified_diff(
        before_lines,
        after_lines,
        fromfile=f"a/{display_path}",
        tofile=f"b/{display_path}",
    )
    return "".join(diff)


def _print_unified_diff(path: pathlib.Path, before: bytes | None, after: bytes | None) -> None:
    print(_get_unified_diff(path, before, after), end="")


def _format_lint_failure_detail(summary: str, detail: str | None = None) -> str:
    detail_body = (detail or "").rstrip()
    if not detail_body:
        return summary
    return f"{summary}\n\n{detail_body}"


def _format_bazel_run_command(target: str, args: list[str]) -> str:
    command = ["bazel", "run", target]
    if args:
        command.extend(["--", *args])
    return shlex.join(command)


def _parse_rules_lint_report(report: str) -> tuple[str, str] | None:
    normalized_report = report.replace("\\", "/")
    report_relative_path = normalized_report.split("/bin/", 1)[-1]
    match = re.fullmatch(
        r"(?:(?P<package>.+)/)?(?P<target>[^/]+)\.AspectRulesLint(?P<linter>[^/.]+)\.out",
        report_relative_path,
    )
    if match is None:
        return None

    package = match.group("package")
    target = match.group("target")
    linter = match.group("linter")
    label = f"//{package}:{target}" if package else f"//:{target}"
    return linter, label


def _extract_actionable_report_line(file_contents: str) -> str | None:
    lines = [line.strip() for line in file_contents.splitlines() if line.strip()]
    if not lines:
        return None

    actionable_patterns = (
        re.compile(r"\.[A-Za-z0-9]+:\d"),
        re.compile(r"[/\\][^:\s]+\.[A-Za-z0-9_]+"),
    )
    for line in lines:
        if any(pattern.search(line) for pattern in actionable_patterns):
            return line

    return lines[0]


def _summarize_rules_lint_failure(report: str, file_contents: str) -> str:
    parsed_report = _parse_rules_lint_report(report)
    actionable_line = _extract_actionable_report_line(file_contents)

    if parsed_report is None:
        return actionable_line or f"rules_lint report: {report}"

    linter, label = parsed_report
    if actionable_line:
        return f"{linter} failed for {label}: {actionable_line}"
    return f"{linter} failed for {label}"


def _summarize_failing_rules_lint_reports(failing_reports: list[str]) -> str:
    if not failing_reports:
        return "Failing reports"
    if len(failing_reports) == 1:
        return failing_reports[0]
    return f"{failing_reports[0]} (+{len(failing_reports) - 1} more failing reports)"


def _get_lint_failure_detail_path() -> pathlib.Path | None:
    lint_failure_detail_path = os.environ.get(LINT_FAILURE_DETAIL_ENV_VAR)
    if not lint_failure_detail_path:
        return None
    return pathlib.Path(lint_failure_detail_path)


def _record_lint_failure_detail(detail: str) -> None:
    lint_failure_detail_path = _get_lint_failure_detail_path()
    if lint_failure_detail_path is None:
        return
    lint_failure_detail_path.write_text(detail, encoding="utf-8")


def _record_lint_failure_detail_if_unset(detail: str) -> None:
    lint_failure_detail_path = _get_lint_failure_detail_path()
    if lint_failure_detail_path is None:
        return

    existing_detail = lint_failure_detail_path.read_text(encoding="utf-8").strip()
    if existing_detail:
        return

    lint_failure_detail_path.write_text(detail, encoding="utf-8")


def _clear_lint_failure_detail() -> None:
    _record_lint_failure_detail("")


def _get_buildozer() -> Optional[str]:
    """Get the path to buildozer, installing it if necessary."""
    from buildscripts.install_bazel import install_bazel

    buildozer_name = "buildozer" if platform.system() != "Windows" else "buildozer.exe"
    buildozer = shutil.which(buildozer_name)
    if not buildozer:
        buildozer = str(pathlib.Path(f"~/.local/bin/{buildozer_name}").expanduser())
        if not os.path.exists(buildozer):
            bazel_bin_dir = str(pathlib.Path("~/.local/bin").expanduser())
            if not os.path.exists(bazel_bin_dir):
                os.makedirs(bazel_bin_dir)
            install_bazel(bazel_bin_dir)
    return buildozer if os.path.exists(buildozer) else None


SUPPORTED_EXTENSIONS = (
    ".bazel",
    ".cpp",
    ".c",
    ".h",
    ".hpp",
    ".py",
    ".js",
    ".mjs",
    ".json",
    ".lock",
    ".toml",
    ".defs",
    ".inl",
    ".idl",
    ".yml",
    ".yaml",
    ".md",
)


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


def list_files_with_targets(bazel_bin: str) -> list:
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
        files_with_targets: list[str],
        type_name: str,
        ext: str,
        dirs: list[str],
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
        }

        exempted_subpaths = [
            # Skip files in bazel_rules_mongo, since it has its own Bazel repo
            "bazel_rules_mongo",
            # vim creates temporary c++ files that aren't part of the tree
            "/.vim/",
        ]

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
                if not any(subpath in file for subpath in exempted_subpaths):
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
        else:
            print(f"All {type_name} files have BUILD.bazel targets!")

    def run_bazel(self, target: str, args: list = []):
        p = subprocess.run([self.bazel_bin, "run", target] + (["--"] + args if args else []))
        if p.returncode != 0:
            self.fail = True
            if not self.keep_going:
                raise LinterFail("Linter failed")

    def refresh_module_lockfile(
        self,
        *,
        fix: bool,
        dry_run: bool,
        lockfile_path: pathlib.Path | None = None,
    ) -> None:
        lockfile_path = lockfile_path or REPO_ROOT / "MODULE.bazel.lock"
        lockfile_display = _display_path(lockfile_path)
        original_contents = _read_optional_bytes(lockfile_path)

        print(f"Refreshing {lockfile_display}...")
        result = subprocess.run(
            [self.bazel_bin, "mod", "deps", "--lockfile_mode=refresh"],
            check=False,
            stdout=sys.stdout,
            stderr=sys.stderr,
        )

        refreshed_contents = _read_optional_bytes(lockfile_path)
        changed = refreshed_contents != original_contents

        if result.returncode != 0:
            if changed:
                _restore_optional_bytes(lockfile_path, original_contents)
            self.fail = True
            if not self.keep_going:
                raise LinterFail(f"Failed to refresh {lockfile_display}")
            return

        if dry_run:
            if not changed:
                print(f"{lockfile_display} is up to date.")
                return
            print(
                f"{lockfile_display} would be updated by `bazel mod deps --lockfile_mode=refresh`:"
            )
            _print_unified_diff(lockfile_path, original_contents, refreshed_contents)
            _restore_optional_bytes(lockfile_path, original_contents)
            return

        if fix:
            if changed:
                print(f"Updated {lockfile_display} via `bazel mod deps --lockfile_mode=refresh`.")
            else:
                print(f"{lockfile_display} is up to date.")
            return

        if not changed:
            print(f"{lockfile_display} is up to date.")
            return

        summary = f"{lockfile_display} has diffs after refresh"
        diff = _get_unified_diff(lockfile_path, original_contents, refreshed_contents)
        print(f"{lockfile_display} has diffs after `bazel mod deps --lockfile_mode=refresh`.")
        if _get_lint_failure_detail_path() is not None:
            _record_lint_failure_detail(_format_lint_failure_detail(summary, diff))
        elif diff:
            print(diff, end="")
        print("Run the following to attempt to fix the issue automatically:")
        print("\tbazel run lint --fix")
        self.fail = True
        if not self.keep_going:
            raise LinterFail(summary)

    def simple_file_size_check(self, files_to_lint: list[str]):
        for file in files_to_lint:
            if not os.path.isfile(file):
                continue
            if os.path.getsize(file) > LARGE_FILE_THRESHOLD:
                print(f"File {file} exceeds large file threshold of {LARGE_FILE_THRESHOLD}")
                self.fail = True
                if not self.keep_going:
                    raise LinterFail("File too large")

    def check_duplicate_lib_names(self):
        """Check for duplicate mongo_cc_library names using buildozer."""
        print("Checking for duplicate cc_library names...")

        buildozer = _get_buildozer()
        if not buildozer:
            self.fail = True
            if not self.keep_going:
                raise LinterFail("buildozer not found")

        # Query all mongo_cc_library targets for their label, name, and srcs
        p = subprocess.run(
            [buildozer, "print label name srcs", "//src/...:%mongo_cc_library"],
            capture_output=True,
            text=True,
        )

        if p.returncode != 0:
            print("buildozer query failed:")
            print(p.stderr)
            self.fail = True
            if not self.keep_going:
                raise LinterFail("buildozer query failed")
            return

        # Parse output and check for duplicates
        # Output format: "//path/to:target name [srcs...]" or "... name (missing)" per line
        # Libraries with no srcs or srcs=(missing) are header-only and don't produce .so
        name_to_labels: dict = {}  # name -> list of labels
        for line in p.stdout.strip().splitlines():
            line = line.strip()
            if not line or line.startswith("rule "):
                # Skip empty lines and "rule ... has no attribute" warnings
                continue

            parts = line.split()
            if len(parts) < 2:
                continue

            label = parts[0]
            name = parts[1]

            # Check if library has source files (produces .so)
            # Format is either: "label name (missing)" for no srcs
            # or: "label name [file1.cpp file2.cpp ...]" for srcs
            has_cpp_sources = False
            if len(parts) > 2:
                # Check if srcs contains .cpp files
                srcs_str = " ".join(parts[2:])
                cpp_extensions = [".cpp", ".cc", ".cxx", ".c++", ".c"]
                if srcs_str != "(missing)" and any(ext in srcs_str for ext in cpp_extensions):
                    has_cpp_sources = True

            if has_cpp_sources:
                if name not in name_to_labels:
                    name_to_labels[name] = []
                name_to_labels[name].append(label)

        # Find duplicates
        duplicates = {name: labels for name, labels in name_to_labels.items() if len(labels) > 1}

        if duplicates:
            error_msg = "Duplicate cc_library names detected:\n\n"
            for name in sorted(duplicates.keys()):
                labels = duplicates[name]
                error_msg += f"  Library name: '{name}'\n"
                for i, label in enumerate(labels):
                    if i == 0:
                        error_msg += f"    First defined at:  {label}\n"
                    else:
                        error_msg += f"    Also defined at:   {label}\n"
                error_msg += "\n"
            error_msg += "When doing dynamic linking, only one .so file with each name can exist.\n"
            error_msg += "This causes the later library to overwrite the first, leading to\n"
            error_msg += "hard-to-debug linking issues at runtime.\n"
            error_msg += "\nPlease rename one of the libraries to avoid this conflict.\n"
            print(error_msg)
            self.fail = True
            if not self.keep_going:
                raise LinterFail("Duplicate cc_library names detected")
        else:
            print("No duplicate cc_library names found!")


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


def _get_files_changed_since_fork_point(origin_branch: str = "origin/master") -> list[str]:
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
        default="auto",
        help="Base branch to compare changes against (example: origin/master).",
    )
    parser.add_argument("--large-files", action="store_true", default=False)
    parser.add_argument(
        "--keep-going",
        action="store_true",
        default=False,
        help="Keep going after failures",
    )
    return parser.parse_known_args(args)


def lint_mod(lint_runner: LintRunner):
    lint_runner.run_bazel("//modules_poc:mod_mapping", ["--validate-modules"])
    # TODO SERVER-122848: add support for the following steps
    # subprocess.run([bazel_bin, "run", "//modules_poc:merge_decls"], check=True)
    # subprocess.run([bazel_bin, "run", "//modules_poc:browse", "--", "merged_decls.json", "--parse-only"], check=True)


def run_rules_lint(bazel_bin: str, args: list[str]):
    parsed_args, args = get_parsed_args(args)
    if platform.system() == "Windows":
        print("eslint not supported on windows")
        raise LinterFail("Unsupported platform")

    if parsed_args.origin_branch == "auto":
        from git import Repo

        from buildscripts.bazel_rules_mongo.utils.evergreen_git import get_mongodb_remote

        remote = get_mongodb_remote(Repo())
        parsed_args.origin_branch = f"{remote.name}/master"

    if parsed_args.fix:
        create_build_files_in_new_js_dirs()

    keep_going = parsed_args.keep_going
    lr = LintRunner(keep_going, bazel_bin)
    lr.refresh_module_lockfile(fix=parsed_args.fix, dry_run=parsed_args.dry_run)

    files_with_targets = list_files_with_targets(bazel_bin)
    lr.list_files_without_targets(files_with_targets, "C++", "cpp", ["src/mongo"])
    lr.list_files_without_targets(files_with_targets, "idl", "idl", ["src"])
    lr.list_files_without_targets(
        files_with_targets,
        "javascript",
        "js",
        ["src/mongo", "jstests"],
    )
    lr.list_files_without_targets(
        files_with_targets,
        "python",
        "py",
        ["src/mongo", "buildscripts", "evergreen"],
    )
    lint_all = parsed_args.all or "..." in args or "//..." in args
    files_to_lint = [arg for arg in args if not arg.startswith("-")]
    if not lint_all and not files_to_lint:
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

    if lint_all or "sbom.private.json" in files_to_lint:
        lr.run_bazel("//buildscripts:sbom_linter")

    if lint_all or any(file.endswith((".h", ".cpp")) for file in files_to_lint):
        lr.run_bazel("//buildscripts:quickmongolint", ["lint"])

    # TODO(SERVER-124155): re-enable once the codebase is free of existing violations
    # if lint_all or any(
    #     file.endswith(
    #         (".cpp", ".c", ".h", ".hpp", ".py", ".js", ".mjs", ".inl", ".idl", ".yml", ".bazel")
    #     )
    #     for file in files_to_lint
    # ):
    #     lr.run_bazel(
    #         "//buildscripts:todo_linter", ["lint-patch", "--branch", parsed_args.origin_branch]
    #     )

    if lint_all or any(
        file.endswith((".cpp", ".c", ".h", ".py", ".idl")) for file in files_to_lint
    ):
        lr.run_bazel("//buildscripts:errorcodes", ["--quiet"])

    if lint_all:
        lr.run_bazel("//buildscripts:pyrightlint", ["lint-all"])
    elif any(file.endswith(".py") for file in files_to_lint):
        lr.run_bazel(
            "//buildscripts:pyrightlint",
            ["lints"] + [str(file) for file in files_to_lint if file.endswith(".py")],
        )

    if lint_all or "poetry.lock" in files_to_lint or "pyproject.toml" in files_to_lint:
        lr.run_bazel("//buildscripts:poetry_lock_check")

    if lint_all or any(file.endswith(".yml") for file in files_to_lint):
        print("Linting evergreen yaml...")
        lr.run_bazel(
            "buildscripts:validate_evg_project_config",
            [
                f"--evg-project-name={parsed_args.lint_yaml_project}",
            ],
        )
        lr.run_bazel("//buildscripts:yamllinters")
        print("No errors found in evergreen yaml")

    if lint_all or any(
        "jstests/streams" in file or "resmokeconfig/suites/streams" in file
        for file in files_to_lint
    ):
        lr.run_bazel("//buildscripts:streams_suite_coverage_linter")

    if lint_all or any(file.endswith(".md") for file in files_to_lint):
        lr.run_bazel("//buildscripts:markdown_link_linter", ["--root=src/mongo", "--verbose"])

    if lint_all or parsed_args.large_files:
        lr.run_bazel("buildscripts:large_file_check", ["--exclude", "src/third_party/*"])
    else:
        lr.simple_file_size_check(files_to_lint)

    if lint_all or any(
        file.endswith((".cpp", ".c", ".h", ".hpp", ".idl", ".inl", ".defs"))
        for file in files_to_lint
    ):
        lint_mod(lr)

    if lint_all or any(file.endswith((".bazel")) for file in files_to_lint):
        lr.check_duplicate_lib_names()

    if lr.fail:
        raise LinterFail("Linter(s) failed")

    # Default to linting everything in rules_lint if no path was passed in.
    if len([arg for arg in args if not arg.startswith("--")]) == 0:
        args = ["//..."] + args

    fix = ""
    buildevents_fd, buildevents_path = tempfile.mkstemp()
    os.close(buildevents_fd)

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

    # the --dry-run flag must immediately follow the --fix flag
    if parsed_args.dry_run:
        fix = "print"

    args = (
        [arg for arg in args if arg.startswith("--") and arg != "--"]
        + ["--"]
        + [arg for arg in args if not arg.startswith("--")]
    )

    # Parse out the reports from the build events
    filter_expr = '.namedSetOfFiles | values | .files[] | select(.name | endswith($ext)) | ((.pathPrefix | join("/")) + "/" + .name)'

    def _jq_files(ext: str, events_path: str) -> list[str]:
        # jq on windows outputs CRLF which breaks this script. https://github.com/jqlang/jq/issues/92
        # Maybe this could be hermetic with bazel run @aspect_bazel_lib//tools:jq or sth
        return (
            subprocess.run(
                ["jq", "--arg", "ext", ext, "--raw-output", filter_expr, events_path],
                capture_output=True,
                text=True,
                check=True,
            )
            .stdout.strip()
            .split("\n")
        )

    # Fix pass: run with fix mode enabled to generate and apply/print patches.
    # This is a separate build from the check pass below because rules_lint's human
    # output in fix mode reports the violations that *were* fixed (pre-fix state), not
    # the violations that *remain* (post-fix state). The check pass re-lints the patched
    # files to produce an accurate report of unfixable violations.
    # See unresolved decision upstream: https://github.com/aspect-build/rules_lint/blob/v2.5.0/lint/ruff.bzl
    # (same TODO exists in eslint.bzl): "if we run with --fix, this will report the
    # issues that were fixed. Does a machine reader want to know about them?"
    if fix:
        fix_buildevents_fd, fix_buildevents_path = tempfile.mkstemp()
        os.close(fix_buildevents_fd)

        fix_args = [
            arg.replace(
                f"--build_event_json_file={buildevents_path}",
                f"--build_event_json_file={fix_buildevents_path}",
            )
            for arg in args
        ]
        sep = fix_args.index("--")
        fix_args = (
            fix_args[:sep]
            + ["--@aspect_rules_lint//lint:fix", "--output_groups=rules_lint_patch"]
            + fix_args[sep:]
        )

        subprocess.run(
            [bazel_bin, "build"] + fix_args, check=True, stdout=sys.stdout, stderr=sys.stderr
        )

        for patch in _jq_files(".patch", fix_buildevents_path):
            if "coverage.dat" in patch or not os.path.exists(patch) or not os.path.getsize(patch):
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

    # Check pass: always run without fix mode to find remaining violations.
    # Runs after the fix pass (if any) so that auto-fixed violations are no longer
    # reported, but unfixable violations still cause a non-zero exit.
    subprocess.run([bazel_bin, "build"] + args, check=True, stdout=sys.stdout, stderr=sys.stderr)

    failing_reports = 0
    for report in _jq_files(".out", buildevents_path):
        if "coverage.dat" in report or not os.path.exists(report) or not os.path.getsize(report):
            continue
        with open(report, "r", encoding="utf-8") as f:
            file_contents = f.read().strip()
            if file_contents == "All checks passed!":
                continue
            print(f"From {report}:")
            print(file_contents)
            print()
            failing_reports += 1

    if failing_reports != 0:
        raise LinterFail("Failing reports")
