import os
import pathlib
import platform
import subprocess
import sys
import tempfile
from typing import List

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))


def check_for_missing_test_stubs():
    bazel_tests = (
        subprocess.check_output(
            [
                "bazel",
                "query",
                "attr(tags, 'mongo_unittest', //...) intersect attr(tags, 'final_target', //...)",
            ],
            stderr=subprocess.DEVNULL,
        )
        .decode("utf-8")
        .splitlines()
    )
    bazel_tests = [bazel_test.split(":")[1] for bazel_test in bazel_tests]

    scons_targets = (
        subprocess.check_output(
            ["grep -rPo 'target\s*=\s*\"\K\w*' ./src  | awk -F: '{print $2}'"],
            stderr=subprocess.STDOUT,
            shell=True,
        )
        .decode("utf-8")
        .splitlines()
    )

    missing_tests = []
    for bazel_test in bazel_tests:
        if bazel_test not in scons_targets:
            missing_tests += [bazel_test]

    if len(missing_tests) == 0:
        print("All bazel tests have SConscript stubs")
        return True

    print("Tests found without SConscript stubs:")
    for missing_test in missing_tests:
        print(missing_test)
    print("\nPlease add a stub in the SConscript file in the directory of each test similar to:")
    print("""
env.CppUnitTest(
    target="test_name",
    source=[],
)

""")
    return False


def create_build_files_in_new_js_dirs():
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
                            build_file.write("""load("@aspect_rules_js//js:defs.bzl", "js_library")

js_library(
    name = "all_javascript_files",
    srcs = glob([
        "*.js",
    ]),
    target_compatible_with = select({
        "//bazel/config:ppc_or_s390x": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
""")
                        print(f"Created BUILD.bazel in {full_dir}")


def list_files_with_targets(bazel_bin: str):
    return [
        line.strip()
        for line in subprocess.run(
            [bazel_bin, "query", 'kind("source file", deps(//...))', "--keep_going"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.splitlines()
    ]


def list_files_without_targets(
    files_with_targets: List[str],
    type_name: str,
    ext: str,
    dirs: List[str],
):
    # rules_lint only checks files that are in targets, verify that all files in the source tree
    # are contained within targets.

    exempt_list = {
        # TODO(SERVER-101365): Remove the exemptions below once resolved.
        "src/mongo/crypto/fle_options.cpp",
        # TODO(SERVER-101361): Remove the exemptions below once resolved.
        "src/mongo/db/auth/authz_manager_external_state_local.cpp",
        "src/mongo/db/auth/authz_manager_external_state_s.cpp",
        # TODO(SERVER-101362): Remove the exemptions below once resolved.
        "src/mongo/db/exec/expression/evaluate_index_test.cpp",
        # TODO(SERVER-101364): Remove the exemptions below once resolved.
        "src/mongo/db/ftdc/ftdc_system_stats_freebsd.cpp",
        "src/mongo/db/ftdc/ftdc_system_stats_macOS.cpp",
        "src/mongo/db/ftdc/ftdc_system_stats_openbsd.cpp",
        "src/mongo/db/ftdc/ftdc_system_stats_solaris.cpp",
        # TODO(SERVER-101365): Remove the exemptions below once resolved.
        "src/mongo/db/service_entry_point_test_fixture.cpp",
        # TODO(SERVER-101366): Remove the exemptions below once resolved.
        "src/mongo/db/read_concern_test.cpp",
        # TODO(SERVER-101367): Remove the exemptions below once resolved.
        "src/mongo/db/timeseries/timeseries_collmod_test.cpp",
        # TODO(SERVER-101368): Remove the exemptions below once resolved.
        "src/mongo/db/modules/enterprise/src/streams/commands/update_connection.cpp",
        # TODO(SERVER-101370): Remove the exemptions below once resolved.
        "src/mongo/db/modules/enterprise/src/streams/third_party/mongocxx/dist/mongocxx/test_util/client_helpers.cpp",
        # TODO(SERVER-101371): Remove the exemptions below once resolved.
        "src/mongo/db/modules/enterprise/src/streams/util/tests/concurrent_memory_aggregator_test.cpp",
        # TODO(SERVER-101372): Remove the exemptions below once resolved.
        "src/mongo/executor/network_interface_perf_test.cpp",
        # TODO(SERVER-101373): Remove the exemptions below once resolved.
        "src/mongo/executor/network_interface_thread_pool_test.cpp",
        # TODO(SERVER-101375): Remove the exemptions below once resolved.
        "src/mongo/platform/decimal128_dummy.cpp",
        # TODO(SERVER-101376): Remove the exemptions below once resolved.
        "src/mongo/platform/stack_locator_emscripten.cpp",
        "src/mongo/platform/stack_locator_freebsd.cpp",
        "src/mongo/platform/stack_locator_macOS.cpp",
        "src/mongo/platform/stack_locator_openbsd.cpp",
        "src/mongo/platform/stack_locator_solaris.cpp",
        "src/mongo/platform/stack_locator_unknown.cpp",
        # TODO(SERVER-101377): Remove the exemptions below once resolved.
        "src/mongo/util/icu_init_stub.cpp",
        # TODO(SERVER-101378): Remove the exemptions below once resolved.
        "src/mongo/util/regex_util.cpp",
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
        return False

    print(f"All {type_name} files have BUILD.bazel targets!")
    return True


def run_rules_lint(bazel_bin, args):
    if platform.system() == "Windows":
        print("eslint not supported on windows")
        sys.exit(1)

    if "--fix" in args:
        create_build_files_in_new_js_dirs()

    files_with_targets = list_files_with_targets(bazel_bin)
    if not list_files_without_targets(files_with_targets, "C++", "cpp", ["src/mongo"]):
        sys.exit(1)

    if not list_files_without_targets(
        files_with_targets, "javascript", "js", ["src/mongo", "jstests"]
    ):
        sys.exit(1)

    if not check_for_missing_test_stubs():
        exit(1)

    # Default to linting everything if no path was passed in
    if len([arg for arg in args if not arg.startswith("--")]) == 0:
        args = ["//..."] + args

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
    if "--fail-on-violation" in args:
        args.extend(["--@aspect_rules_lint//lint:fail_on_violation", "--keep_going"])
        args.remove("--fail-on-violation")

    # Allow a `--fix` option on the command-line.
    # This happens to make output of the linter such as ruff's
    # [*] 1 fixable with the `--fix` option.
    # so that the naive thing of pasting that flag to lint.sh will do what the user expects.
    if "--fix" in args:
        fix = "patch"
        args.extend(["--@aspect_rules_lint//lint:fix", "--output_groups=rules_lint_patch"])
        args.remove("--fix")

    # the --dry-run flag must immediately follow the --fix flag
    if "--dry-run" in args:
        fix = "print"
        args.remove("--dry-run")

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
        failing_reports += 1
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
    elif failing_reports != 0:
        sys.exit(1)
