import os
import pathlib
import platform
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))


def list_files_without_targets(bazel_bin: str):
    # rules_lint only checks files that are in targets, verify that all files in the source tree
    # are contained within targets.

    js_files_in_targets = [
        line.strip()
        for line in subprocess.check_output(
            [bazel_bin, "cquery", 'kind("source file", deps(//...))', "--output", "files"],
            stderr=subprocess.STDOUT,
        )
        .decode("utf-8")
        .splitlines()
        if line.strip().endswith("js")
    ]

    print("Checking that all javascript files have BUILD.bazel targets...")

    # Find all .js files in src/mongo and jstests
    js_files = (
        subprocess.check_output(
            ["find", "src/mongo", "jstests", "-name", "*.js"],
            stderr=subprocess.STDOUT,
        )
        .decode("utf-8")
        .splitlines()
    )

    # Convert js_files_in_targets to a set for easy comparison
    js_files_in_targets_set = set()
    for file in js_files_in_targets:
        # Remove the leading "//" and replace ":" with "/"
        clean_file = file.lstrip("//").replace(":", "/")
        js_files_in_targets_set.add(clean_file)

    # Create a new list of files that are in js_files but not in js_files_in_targets
    new_list = []
    for file in js_files:
        if file not in js_files_in_targets_set:
            new_list.append(file)

    if len(new_list) != 0:
        print("Found javascript files without BUILD.bazel definitions:")
        for file in new_list:
            print(f"\t{file}")
        print("")
        print("Please add these to a js_library target in a BUILD.bazel file in their directory")
        return False

    print("All javascript files have BUILD.bazel targets!")
    return True


def run_rules_lint(bazel_bin, args):
    if platform.system() == "Windows":
        print("eslint not supported on windows")
        sys.exit(1)

    if not list_files_without_targets(bazel_bin):
        sys.exit(1)

    # Default to linting everything if no path was passed in
    if len(args) == 0:
        args = ["//..."]

    fix = ""
    with tempfile.NamedTemporaryFile(delete=False) as buildevents:
        buildevents_path = buildevents.name

    args.append("--aspects=//tools/lint:linters.bzl%eslint")

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
    if args[1] == "--fail-on-violation":
        args.extend(["--@aspect_rules_lint//lint:fail_on_violation", "--keep_going"])
        args.pop(1)

    # Allow a `--fix` option on the command-line.
    # This happens to make output of the linter such as ruff's
    # [*] 1 fixable with the `--fix` option.
    # so that the naive thing of pasting that flag to lint.sh will do what the user expects.
    if args[1] == "--fix":
        fix = "patch"
        args.extend(["--@aspect_rules_lint//lint:fix", "--output_groups=rules_lint_patch"])
        args.pop(1)

    # the --dry-run flag must immediately follow the --fix flag
    if args[1] == "--dry-run":
        fix = "print"
        args.pop(1)

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

    for report in valid_reports:
        # Exclude coverage reports, and check if the output is empty.
        if "coverage.dat" in report or not os.path.exists(report) or not os.path.getsize(report):
            # Report is empty. No linting errors.
            continue
        print(f"From {report}:")
        with open(report, "r", encoding="utf-8") as f:
            print(f.read())
        print()

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
                sys.exit(1)
