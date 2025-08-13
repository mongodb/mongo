import argparse
import hashlib
import json
import os
import platform
import stat
import subprocess
import sys
import time
import urllib.request
from collections import deque
from pathlib import Path
from typing import Dict, List

from retry import retry

sys.path.append(".")

from buildscripts.install_bazel import install_bazel
from buildscripts.simple_report import make_report, put_report, try_combine_reports

RELEASE_URL = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/"

groups_sort_keys = {
    "first": 1,
    "second": 2,
    "third": 3,
    "fourth": 4,
    "fifth": 5,
    "sixth": 6,
    "seventh": 7,
    "eighth": 8,
}


@retry(tries=3, delay=5)
def _download_with_retry(*args, **kwargs):
    return urllib.request.urlretrieve(*args, **kwargs)


def determine_platform():
    syst = platform.system()
    pltf = None
    if syst == "Darwin":
        pltf = "darwin"
    elif syst == "Windows":
        pltf = "windows"
    elif syst == "Linux":
        pltf = "linux"
    else:
        raise RuntimeError("Platform cannot be inferred.")
    return pltf


def determine_architecture():
    arch = None
    machine = platform.machine()
    if machine in ("AMD64", "x86_64"):
        arch = "amd64"
    elif machine in ("arm", "arm64", "aarch64"):
        arch = "arm64"
    else:
        raise RuntimeError(f"Detected architecture is not supported: {machine}")

    return arch


def download_buildozer(download_location: str = "./"):
    operating_system = determine_platform()
    architechture = determine_architecture()
    if operating_system == "windows" and architechture == "arm64":
        raise RuntimeError("There are no published arm windows releases for buildozer.")

    extension = ".exe" if operating_system == "windows" else ""
    binary_name = f"buildozer-{operating_system}-{architechture}{extension}"
    url = f"{RELEASE_URL}{binary_name}"

    file_location = os.path.join(download_location, f"buildozer{extension}")
    _download_with_retry(url, file_location)
    os.chmod(file_location, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
    return file_location


def find_group(unittest_paths):
    groups = {
        # group1
        "0": "first",
        "1": "first",
        # group2
        "2": "second",
        "3": "second",
        # group3
        "4": "third",
        "5": "third",
        # group4
        "6": "fourth",
        "7": "fourth",
        # group5
        "8": "fifth",
        "9": "fifth",
        # group6
        "a": "sixth",
        "b": "sixth",
        # group7
        "c": "seventh",
        "d": "seventh",
        # group8
        "e": "eighth",
        "f": "eighth",
    }

    group_to_path: Dict[str, List[str]] = {}

    for path in unittest_paths:
        norm_path = path.replace(":", "/").replace("\\", "/")
        if norm_path.startswith("//"):
            norm_path = norm_path[2:]
        if not norm_path.startswith("src/"):
            print(f"ERROR: {path} not relative to mongo repo root")
            sys.exit(1)

        basename = os.path.basename(norm_path)

        if basename.startswith("lib"):
            basename = basename[3:]

        ext = basename.find(".")
        if ext != -1:
            basename = basename[:ext]
        dirname = os.path.dirname(norm_path)

        hash_path = os.path.join(dirname, basename).replace("\\", "/")
        first_char = hashlib.sha256(hash_path.encode()).hexdigest()[0]
        group = groups[first_char]
        if group not in group_to_path:
            group_to_path[group] = []
        group_to_path[group].append(path)

    return json.dumps(group_to_path, indent=4)


def find_multiple_groups(test, groups):
    tagged_groups = []
    for group in groups:
        if test in groups[group]:
            tagged_groups.append(group)
    return tagged_groups


def iter_clang_tidy_files(root: str | Path) -> list[Path]:
    """Return a list of repo-relative Paths to '.clang-tidy' files.
    - Uses os.scandir for speed
    - Does NOT follow symlinks
    """
    root = Path(root).resolve()
    results: list[Path] = []
    stack = deque([root])

    while stack:
        current = stack.pop()
        try:
            with os.scandir(current) as it:
                for entry in it:
                    name = entry.name
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(Path(entry.path))
                    elif entry.is_file(follow_symlinks=False) and name == ".clang-tidy":
                        # repo-relative path
                        results.append(Path(entry.path).resolve().relative_to(root))
        except PermissionError:
            continue
    return results

def validate_clang_tidy_configs(generate_report, fix):
    buildozer = download_buildozer()

    mongo_dir = "src/mongo"

    tidy_files = iter_clang_tidy_files("src/mongo")

    p = subprocess.run(
        [buildozer, "print label srcs", "//:clang_tidy_config_files"],
        capture_output=True,
        text=True,
    )
    tidy_targets = None
    for line in p.stdout.splitlines():
        if line.startswith("//") and line.endswith("]"):
            tokens = line.split("[")
            tidy_targets = tokens[1][:-1].split(" ")
            break
    if tidy_targets is None:
        print(p.stderr)
        raise Exception(f"could not parse tidy config targets from '{p.stdout}'")

    if tidy_targets == ['']:
        tidy_targets = []

    all_targets = []
    for tidy_file in tidy_files:
        tidy_file_target = "//" + os.path.dirname(os.path.join(mongo_dir, tidy_file)) + ":clang_tidy_config"
        all_targets.append(tidy_file_target)
    
    if all_targets != tidy_targets:
        msg = f"Incorrect clang tidy config targets: {all_targets} != {tidy_targets}"
        print(msg)
        if generate_report:
            report = make_report("//:clang_tidy_config_files", msg, 1)
            try_combine_reports(report)
            put_report(report)

    if fix:
        subprocess.run([buildozer, f"set srcs {' '.join(all_targets)}", "//:clang_tidy_config_files"])


def validate_bazel_groups(generate_report, fix):
    buildozer = download_buildozer()

    bazel_bin = install_bazel(".")

    query_opts = [
        "--implicit_deps=False",
        "--tool_deps=False",
        "--include_aspects=False",
        "--bes_backend=",
        "--bes_results_url=",
    ]

    try:
        start = time.time()
        sys.stdout.write("Query all unittest binaries... ")
        sys.stdout.flush()
        query_proc = subprocess.run(
            [
                bazel_bin,
                "query",
                'kind(extract_debug, attr(tags, "[\[ ]mongo_unittest[,\]]", //src/...))',
            ]
            + query_opts,
            capture_output=True,
            text=True,
            check=True,
        )
        bazel_unittests = query_proc.stdout.splitlines()
        sys.stdout.write("{:0.2f}s\n".format(time.time() - start))
    except subprocess.CalledProcessError as exc:
        print("BAZEL ERROR:")
        print(exc.stdout)
        print(exc.stderr)
        sys.exit(exc.returncode)

    buildozer_update_cmds = []

    groups = json.loads(find_group(bazel_unittests))
    failures = []
    for group in sorted(groups, key=lambda x: groups_sort_keys[x]):
        try:
            start = time.time()
            sys.stdout.write(f"Query all mongo_unittest_{group}_group unittests... ")
            sys.stdout.flush()
            query_proc = subprocess.run(
                [
                    bazel_bin,
                    "query",
                    f'kind(extract_debug, attr(tags, "[\[ ]mongo_unittest_{group}_group[,\]]", //src/...))',
                ]
                + query_opts,
                capture_output=True,
                text=True,
                check=True,
            )
            sys.stdout.write("{:0.2f}s\n".format(time.time() - start))
            group_tests = query_proc.stdout.splitlines()
        except subprocess.CalledProcessError as exc:
            print("BAZEL ERROR:")
            print(exc.stdout)
            print(exc.stderr)
            sys.exit(exc.returncode)

        if groups[group] != group_tests:
            for test in group_tests:
                if test not in bazel_unittests:
                    failures.append(
                        [
                            test + " tag",
                            f"{test} not a 'mongo_unittest' but has 'mongo_unittest_{group}_group' tag.",
                        ]
                    )
                    print(failures[-1][1])
                    if fix:
                        buildozer_update_cmds += [
                            [f"remove tags mongo_unittest_{group}_group", test]
                        ]

            for test in groups[group]:
                if test not in group_tests:
                    failures.append(
                        [test + " tag", f"{test} missing 'mongo_unittest_{group}_group'"]
                    )
                    print(failures[-1][1])
                    if fix:
                        buildozer_update_cmds += [[f"add tags mongo_unittest_{group}_group", test]]

            for test in group_tests:
                if test not in groups[group]:
                    failures.append(
                        [
                            test + " tag",
                            f"{test} is tagged in the wrong group.",
                        ]
                    )
                    print(failures[-1][1])
                    if fix:
                        buildozer_update_cmds += [
                            [f"remove tags mongo_unittest_{group}_group", test]
                        ]

    if fix:
        for cmd in buildozer_update_cmds:
            subprocess.run([buildozer] + cmd)

    if failures:
        for failure in failures:
            if generate_report:
                report = make_report(failure[0], failure[1], 1)
                try_combine_reports(report)
                put_report(report)


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument("--generate-report", default=False, action="store_true")
    parser.add_argument("--fix", default=False, action="store_true")
    args = parser.parse_args()
    validate_clang_tidy_configs(args.generate_report, args.fix)
    validate_bazel_groups(args.generate_report, args.fix)


if __name__ == "__main__":
    main()
