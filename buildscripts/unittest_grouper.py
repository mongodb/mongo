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
from typing import Dict, List

from buildscripts.simple_report import make_report, put_report, try_combine_reports

sys.path.append(".")

from buildscripts.install_bazel import install_bazel

RELEASE_URL = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/"


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
        raise RuntimeError("There are no published arm windows releases for buildifier.")

    extension = ".exe" if operating_system == "windows" else ""
    binary_name = f"buildozer-{operating_system}-{architechture}{extension}"
    url = f"{RELEASE_URL}{binary_name}"

    file_location = os.path.join(download_location, f"buildozer{extension}")
    urllib.request.urlretrieve(url, file_location)
    os.chmod(file_location, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
    return file_location


def find_group(unittest_paths):
    groups = {
        # group1
        "0": "first",
        "1": "first",
        "2": "first",
        "3": "first",
        # group2
        "4": "second",
        "5": "second",
        "6": "second",
        "7": "second",
        # group3
        "8": "third",
        "9": "third",
        "a": "third",
        "b": "third",
        # group4
        "c": "fourth",
        "d": "fourth",
        "e": "fourth",
        "f": "fourth",
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


def validate_bazel_groups(generate_report, fix, quick):
    buildozer = download_buildozer()

    bazel_bin = install_bazel(".")

    if quick:
        print(
            "Checking unittests in quick mode, you may consider running 'fix-unittests' for a thorough (longer) check."
        )

    query_opts = [
        "--implicit_deps=False",
        "--tool_deps=False",
        "--include_aspects=False",
        "--remote_executor=",
        "--remote_cache=",
        "--bes_backend=",
        "--bes_results_url=",
    ]
    try:
        start = time.time()
        sys.stdout.write("Query all unittest runner rules... ")
        sys.stdout.flush()
        query_proc = subprocess.run(
            [
                bazel_bin,
                "cquery",
                'kind("mongo_install_rule", //:all-targets)',
                "--output",
                "build",
            ]
            + query_opts,
            capture_output=True,
            text=True,
            check=True,
        )
        sys.stdout.write("{:0.2f}s\n".format(time.time() - start))
        installed_tests = query_proc.stdout.splitlines()
        installed_test_names = set()
        for i in range(2, len(installed_tests)):
            if 'generator_function = "mongo_unittest_install' in installed_tests[i]:
                test_name = installed_tests[i - 1].split('"')[1]
                installed_test_names.add(test_name)

    except subprocess.CalledProcessError as exc:
        print("BAZEL ERROR:")
        print(exc.stdout)
        print(exc.stderr)
        sys.exit(exc.returncode)

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

    buildozer_delete_cmds = []
    buildozer_add_cmds = []
    buildozer_update_cmds = []
    nodiff_build_files = set()
    diff_build_files = set()

    for test in bazel_unittests:
        test_name = test.split(":")[1]
        if test_name not in installed_test_names:
            buildozer_add_cmds += [f"new mongo_unittest_install {test_name}"]
            buildozer_update_cmds += [[f"add srcs {test}", f"//:{test_name}"]]
        else:
            installed_test_names.remove(test_name)

        if quick:
            build_file = (
                os.path.dirname(test.replace("//", "./").replace(":", "/")) + "/BUILD.bazel"
            )
            if build_file not in diff_build_files and build_file not in nodiff_build_files:
                cmd = [
                    "git",
                    "diff",
                    "master",
                    os.path.dirname(test.replace("//", "./").replace(":", "/")) + "/BUILD.bazel",
                ]
                stdout = subprocess.run(cmd, capture_output=True, text=True).stdout

            if build_file in nodiff_build_files or not stdout:
                nodiff_build_files.add(build_file)
                associated_files = []
                # print(installed_test_names)
                for name in installed_test_names:
                    if name.startswith(test_name + "-"):
                        associated_files.append(name)
                for name in associated_files:
                    installed_test_names.remove(name)
                continue
            else:
                diff_build_files.add(build_file)

        try:
            start = time.time()
            sys.stdout.write(f"Query tests in {test}... ")
            sys.stdout.flush()
            query_proc = subprocess.run(
                [
                    bazel_bin,
                    "query",
                    f"labels(srcs, {test}_with_debug)",
                ]
                + query_opts,
                capture_output=True,
                text=True,
                check=True,
            )
            sys.stdout.write("{:0.2f}s\n".format(time.time() - start))
            sources = query_proc.stdout.splitlines()

        except subprocess.CalledProcessError as exc:
            print("BAZEL ERROR:")
            print(exc.stdout)
            print(exc.stderr)
            sys.exit(exc.returncode)

        for source in sources:
            if source.endswith(".cpp"):
                source_base = source.split(":")
                if len(source_base) > 1:
                    source_base = source_base[1]
                else:
                    source_base = source_base[0]
                source_base = source_base.replace(".cpp", "")
                if f"{test_name}-{source_base}" not in installed_test_names:
                    buildozer_add_cmds += [f"new mongo_unittest_install {test_name}-{source_base}"]
                    buildozer_update_cmds += [[f"add srcs {test}", f"//:{test_name}-{source_base}"]]
                else:
                    installed_test_names.remove(f"{test_name}-{source_base}")

    for existing_test in installed_test_names:
        buildozer_delete_cmds += ["delete", existing_test]

    groups = json.loads(find_group(bazel_unittests))
    failures = []
    for group in groups:
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
                if test not in groups[group]:
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
                        buildozer_update_cmds += [
                            [buildozer, f"add tags mongo_unittest_{group}_group", test]
                        ]

    if fix:
        if buildozer_delete_cmds:
            subprocess.run([buildozer] + buildozer_delete_cmds)
        subprocess.run([buildozer] + buildozer_add_cmds + ["//:__pkg__"])
        for cmd in buildozer_update_cmds:
            subprocess.run([buildozer] + cmd)

    if buildozer_delete_cmds or buildozer_add_cmds:
        failures.append(["unittest install rules", "Some install rules are incorrect"])

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
    validate_bazel_groups(args.generate_report, args.fix, quick=False)


if __name__ == "__main__":
    main()
