import glob
import hashlib
import os
import platform
import subprocess
import sys
import tarfile
from urllib.request import urlretrieve

import git
import requests
import retry

from buildscripts.util.read_config import read_config_file


@retry.retry(tries=3, delay=5)
def retry_download(url: str, file: str):
    print(f"Attempting to download {file} from {url}")
    urlretrieve(url, file)


@retry.retry(tries=3, delay=5)
def retry_coveralls_report(args: list):
    print("Running coveralls report...")
    proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8")
    print(proc.stdout)
    if proc.returncode != 0:
        raise RuntimeError("Coveralls report failed, see above process output for details.")


@retry.retry(tries=3, delay=5)
def retry_coveralls_post(coveralls_report: str):
    print("Posting to coveralls")
    files = {"json_file": open(coveralls_report, "rb")}
    response = requests.post("https://coveralls.io/api/v1/jobs", files=files)
    print(response.text)
    if not response.ok:
        raise RuntimeError(f"Error while sending coveralls report: {response.status_code}")


def download_coveralls_reporter(url: str, sha: str):
    print(f"Downloading coveralls from {url}")
    tar_location = "coveralls.tar.gz"
    retry_download(url, tar_location)
    with tarfile.open(tar_location, "r:gz") as tar:
        tar.extractall()
    os.remove(tar_location)

    if not os.path.exists("coveralls"):
        raise RuntimeError(
            "Coveralls was successfully downloaded but the binary was not found in the expected location."
        )

    sha256_hash = hashlib.sha256()
    with open("coveralls", "rb") as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    calculated_sha = sha256_hash.hexdigest()
    if calculated_sha != sha:
        raise RuntimeError(
            f"Downloaded file from {url} calculated sha ({calculated_sha}) did not match expected sha ({sha})"
        )


def main():
    expansions_file = "../expansions.yml"
    expansions = read_config_file(expansions_file)
    gcov_binary = expansions.get("gcov_tool", None)
    if not gcov_binary:
        print(
            "Missing 'gcov_tool' expansion, skipping code coverage because this is likely not a code coverage variant."
        )
        return 0

    should_gather_code_coverage = expansions.get("gather_code_coverage_results", False)
    if not should_gather_code_coverage:
        print("Missing 'gather_code_coverage_results' expansion, skipping code coverage.")
        return 0

    if not os.path.exists(".git"):
        print("No git repo found in working directory. Code coverage needs git repo to function.")
        return 1

    repo = git.Repo()
    head_sha = repo.head.object.hexsha
    evergreen_revision = expansions.get("revision")
    # We need all code coverage runs from the same patch to have the same commit on HEAD
    # When different runs have different git SHAs they are grouped in different builds in coveralls.
    #
    # Because of the weird cloning we do to have a git repo present during code coverage tasks
    # and the inconsistent behaviors of the local git state during evergreen patches and
    # commit-queue runs. We need to always ensure that the HEAD matches the evergreen revision
    # for consistency in coveralls.
    if head_sha != evergreen_revision:
        print("test not equal")
        print(f"head_sha: {head_sha}")
        print(f"evergreen revision: {evergreen_revision}")
        repo.git.reset("--mixed", evergreen_revision)

    disallowed_arches = {"s390x", "s390", "ppc64le", "ppc64", "ppc", "ppcle"}
    arch = platform.uname().machine.lower()
    print(f"Detected arch: {arch}")
    if arch in disallowed_arches:
        print("Code coverage not supported on this architecture")
        return 1

    coveralls_token = expansions.get("coveralls_token")
    assert coveralls_token is not None, "Coveralls token was not found"
    github_pr_number = expansions.get("github_pr_number", "")
    revision_order_id = expansions.get("revision_order_id")
    version_id = expansions.get("version_id")
    task_id = expansions.get("task_id")

    workdir = expansions.get("workdir")
    bazelisk_path = os.path.join(workdir, "tmp", "bazelisk")
    if os.path.exists(bazelisk_path):
        print("Found bazel, looking for output path")
        proc = subprocess.run(
            [bazelisk_path, "info", "output_path"], check=True, capture_output=True
        )
        bazel_output_location = proc.stdout.decode("utf-8").strip()
        bazel_coverage_report_location = os.path.join(
            bazel_output_location, "_coverage", "_coverage_report.dat"
        )
        if os.path.exists(bazel_coverage_report_location):
            print("Found bazel coverage report.")
            coveralls_location = "coveralls"
            if arch == "arm64" or arch == "aarch64":
                coveralls_url = "https://github.com/coverallsapp/coverage-reporter/releases/download/v0.6.15/coveralls-linux-aarch64.tar.gz"
                sha_sum = "47653fa86b8eaae30b16c5d3f37fbeda30d8708153a261df2cdc9cb67e6c48e0"
            else:
                coveralls_url = "https://github.com/coverallsapp/coverage-reporter/releases/download/v0.6.15/coveralls-linux-x86_64.tar.gz"
                sha_sum = "59b159a93ae44a649fe7ef8f10d906c146057c4f81acb4f448d441b9ff4dadb3"
            download_coveralls_reporter(coveralls_url, sha_sum)

            args = [
                os.path.abspath(coveralls_location),
                "report",
                f"--repo-token={coveralls_token}",
                "--service-name=travis-ci",
                f"--build-url=https://spruce.mongodb.com/version/{version_id}/",
                f"--job-id={revision_order_id}",
                f"--job-url=https://spruce.mongodb.com/task/{task_id}/",
            ]
            if github_pr_number:
                args.append(
                    f"--pull-request={github_pr_number}",
                )
            args.append(bazel_coverage_report_location)
            retry_coveralls_report(args)
            # no gcda files are generated from bazel coverage so we can exit early here
            return 0

    scons_build_dir = os.path.join(workdir, "src", "build", "debug")
    if os.path.exists(scons_build_dir):
        has_scons_gcno = any(
            True for _ in glob.iglob("./**/*.gcno", root_dir=scons_build_dir, recursive=True)
        )
    else:
        has_scons_gcno = False

    # because of bazel symlink shenanigans, the bazel gcda and gcno files are put in different
    # directories when the GCOV_PREFIX and GCOV_PREFIX_STRIP env vars are used. We manually
    # put the gcno files where the gcda files are generated to fix this.
    has_bazel_gcno = False
    bazel_output_dir = os.path.join(workdir, "bazel-out")
    for file in glob.iglob("./**/bazel-out/**/*.gcno", root_dir=workdir, recursive=True):
        has_bazel_gcno = True
        parts = file.split("bazel-out/")
        assert len(parts) == 2, "Something went wrong, path was not split into 2 parts."
        old_path = os.path.join(workdir, file)
        new_path = os.path.join(bazel_output_dir, parts[1])
        new_dir = os.path.dirname(new_path)
        os.makedirs(new_dir, exist_ok=True)
        os.rename(old_path, new_path)

    if not has_bazel_gcno and not has_bazel_gcno:
        print("No gcno files were found. Something went wrong.")
        return 1

    my_env = os.environ.copy()
    my_env["COVERALLS_REPO_TOKEN"] = coveralls_token
    my_env["TRAVIS_PULL_REQUEST"] = github_pr_number
    my_env["TRAVIS_JOB_ID"] = revision_order_id

    coveralls_report = "gcovr-coveralls.json"

    args = [
        "python3",
        "-m",
        "gcovr",
        "--output",
        coveralls_report,
        "--coveralls-pretty",
        "--txt",
        "gcovr-coveralls.txt",
        "--print-summary",
        "--exclude",
        "build/debug/.*",
        "--exclude",
        ".*bazel-out/.*",
        "--exclude",
        ".*external/mongo_toolchain/.*",
        "--exclude",
        ".*src/.*_gen\.(h|hpp|cpp)",
        "--exclude",
        ".*src/mongo/db/cst/grammar\.yy",
        "--exclude",
        ".*src/mongo/idl/.*",
        "--exclude",
        ".*src/mongo/.*_test\.(h|hpp|cpp)",
        "--exclude",
        ".*src/mongo/dbtests/.*",
        "--exclude",
        ".*src/mongo/unittest/.*",
        "--exclude",
        ".*/third_party/.*",
        "--gcov-ignore-errors",
        "source_not_found",
        "--gcov-ignore-parse-errors",
        "negative_hits.warn",
        "--gcov-exclude-directories",
        ".*src/mongo/dbtests/.*",
        "--gcov-exclude-directories",
        ".*src/mongo/idl/.*",
        "--gcov-exclude-directories",
        ".*src/mongo/unittest/.*",
        "--gcov-exclude-directories",
        ".*/third_party/.*",
        "--gcov-executable",
        gcov_binary,
    ]

    if has_bazel_gcno:
        args.append(bazel_output_dir)

    if has_scons_gcno:
        args.append(scons_build_dir)

    print("Running gcovr command")
    process = subprocess.run(
        args, env=my_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8"
    )
    print(process.stdout)
    if process.returncode != 0:
        print(f"gcovr failed with code: {process.returncode}")
        return 1

    if not os.path.exists(coveralls_report):
        print("Could not find coveralls json report")
        return 1

    retry_coveralls_post(coveralls_report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
