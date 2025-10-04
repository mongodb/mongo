"""
This script sets up an enviroment to locally reproduce query_correctness_tests failures.
Example usage:
    python3 setup_repo_env.py <task_id> <task_id> ... <task_id>
    task_id can be found in the evergreen task URL, i.e. https://spruce.mongodb.com/task/<task_id>?execution=0&sorts=STATUS%3AASC
Optional Args:
    --evergreen_bin: Path to the evergreen binary. Defaults to /home/ubuntu/evergreen
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List

REPO_NAME_PREFIX = "query-correctness-tests-"


def fetch_evg_logs(task_id: str, evergreen_binary: Path, output_file: Path):
    """
    Fetches the logs for all jobs of a given evergreen task and write it into a specified output file.

    Args:
        task_id: The task_id of the evergreen task.
        evergreen_binary: Path to the evergreen binary.
        output_file: The file to output the logs to.
    """
    # Determine the number of resmoke jobs that evergreen task with task_id is using.
    num_jobs = None
    command = [evergreen_binary, "task", "build", "TaskLogs", "--task_id", task_id]
    for line in subprocess.run(command, capture_output=True, text=True).stdout.split("\n"):
        if match := re.match(r"resmoke_jobs:\s*(?P<njobs>\d+)", line):
            num_jobs = int(match.group("njobs"))
            break

    if not num_jobs:
        sys.exit(f"Error: Could not determine number of jobs for task_id {task_id}")

    # Clear output file if it already exists.
    output_file.unlink(missing_ok=True)

    # Fetch the logs for each of the jobs and concatenate them into a single log file.
    for job_num in range(num_jobs):
        command = [
            evergreen_binary,
            "task",
            "build",
            "TestLogs",
            "--task_id",
            task_id,
            "--log_path",
            f"job{job_num}",
        ]

        # Ensure directories along the path exist.
        output_file.parent.mkdir(parents=True, exist_ok=True)
        with output_file.open("a+") as file:
            result = subprocess.run(command, stdout=file, stderr=subprocess.PIPE, text=True)
        if result.stderr:
            sys.exit(f"Error while fetching evergreen logs:\n{result.stderr}")


def extract_repo_and_subpath(test_path: Path):
    """
    Helper function to extract the test repo name and the remaining subpath given a path to a test file.
    Args:
        test_path: The test path from which we want to extract the test repo name and the remaining subpath.
    Returns:
        Tuple(repo_name, subpath)
    """
    absolute = test_path.absolute()
    for path in absolute.parents:
        if path.name.startswith(REPO_NAME_PREFIX):
            return path.name, absolute.relative_to(path)

    sys.exit(
        f"Error: Failed to find valid repo_name in the failing test path {test_path}. Expected to find a file path with {REPO_NAME_PREFIX}."
    )


def process_log_file(logfile: Path, failing_tests_map: Dict[str, Dict[str, List[int]]]):
    """
    Parses evergreen log files by regex matching relevant parts of the query_tester output to determine the failing test files, and the corresponding test numbers.

    Args:
        logfile: File path to evergreen log.
        failing_tests_map: dictionary that maps the test repo to its failing test files and their failing test numbers. We modify this in-place.
    """
    fail_pattern = re.compile(r"FAIL: (?P<test_path>.*)$")
    testnum_pattern = re.compile(r"TestNum: (\d+)")
    test_output_break_pattern = re.compile(r"^(?!\[query_tester_server_test:)")

    curr_test_file = None
    repo_name = None
    with logfile.open("r") as file:
        for line in file:
            # Check for start of a failing test file block.
            if fail_match := fail_pattern.search(line):
                test_path = Path(fail_match.group("test_path"))

                repo_name, curr_test_file = extract_repo_and_subpath(test_path)

            # Find all the failing test numbers in the curr_test_file_path.
            if curr_test_file:
                if testnum_match := testnum_pattern.search(line):
                    test_num = testnum_match.group(1)
                    failing_tests_map.setdefault(repo_name, {}).setdefault(
                        curr_test_file, []
                    ).append(test_num)

                # Break in query_tester output.
                if test_output_break_pattern.search(line):
                    curr_test_file = None
                    repo_name = None


def create_repo_commit_map(test_repos_conf_file: Path):
    """
    Creates a dictionary that maps test corpus repo name to the commit hash that is pinned to this revision of the server code.

    Args:
        test_repos_conf_file: File path to the test_repos.conf file.
    """
    with test_repos_conf_file.open("r") as f:
        repo_commit_map = dict(
            line.strip().split(":", 1) for line in f if not line.startswith("#") and line.strip()
        )
    if not repo_commit_map:
        sys.exit("Error: No test repo and commit hashes found.")

    return repo_commit_map


def fetch_test_repos_dirs(failing_tests_map, repo_commit_map: Dict[str, str], clone_dir: Path):
    """
    Fetch the query-correctness-tests-* repos with the failing tests, checks out the repos to the correct commit hash and only checks out the failing test files.

    Args:
        failing_tests_map: dictionary that maps the test repo to its failing test files and their failing test numbers. We modify this in-place.
        repo_commit_map: map of query-correctness-tests-repo to pinned commit hash.
        clone_dir: The directory that we want to clone the query-correctness-tests-* repos into.
    """

    for repo_name in failing_tests_map:
        repo_dir = clone_dir / repo_name
        # Ensure none of the repos exist locally
        if repo_dir.exists():
            sys.exit(f"Error: {repo_dir} already exists. Please delete {repo_dir} and try again.")
        # Ensure that repo_name is in the repo_commit_map.
        if repo_name not in repo_commit_map:
            sys.exit(f"Error: Unable to find {repo_name} in test_repos.conf.")

        commit_hash = repo_commit_map[repo_name]

        # Sparse clone the test corpus repo.
        command = [
            "git",
            "clone",
            "-q",
            "--sparse",
            "--filter=blob:none",
            f"git@github.com:10gen/{repo_name}",
            repo_dir,
        ]
        subprocess.run(command)

        # Checkout to the correct commmit hash based on test_repos.conf.
        command = ["git", "-C", repo_dir, "checkout", "-q", commit_hash]
        subprocess.run(command)

        # Determine the failing test files we want to checkout.
        files_to_checkout = []
        for test_file in failing_tests_map[repo_name]:
            test_file = Path(test_file)
            files_to_checkout.extend(
                [test_file, test_file.with_suffix(".results"), test_file.parent / "*.coll"]
            )

        # Use sparse-checkout to only checkout the failing test files.
        command = [
            "git",
            "-C",
            repo_dir,
            "sparse-checkout",
            "set",
            ".gitattributes",
            *files_to_checkout,
            "-q",
        ]
        subprocess.run(command)


def build_query_tester_command(failing_tests_map: Dict[str, Dict[str, List[int]]], out_dir: Path):
    """
    Find all the failing test files from the failing_tests_map and output a formatted string that can be used as an argument to the mongotest binary

    Args:
        failing_tests_map: dictionary that maps the test repo to its failing test files and their failing test numbers. We modify this in-place.
    Returns:
        Formatted string to pass into mongotest to run selected tests.
    """
    # TODO: SERVER-101147 - Use the TestNum information in the failing_tests_map to create more targetted invocation.
    test_files_to_run = []
    for repo_name in failing_tests_map:
        repo_dir = out_dir / repo_name
        for test_file in failing_tests_map[repo_name]:
            # Construct the file path.
            file_path = repo_dir / test_file
            test_files_to_run.append(f"-t {file_path}")
    return " ".join(test_files_to_run)


def discover_test_repos_conf():
    """
    Starting at the current script's directory, it walks the file tree to discover test_repos.conf.

    Returns:
        Path to test_repos.conf
    """
    target_file = "test_repos.conf"
    curr_dir: Path = Path(__file__).resolve().parent

    # Walk up the tree until we hit the HOME directory.
    while curr_dir != Path.home():
        # Iterate through all the piblings.
        for sibling in curr_dir.parent.iterdir():
            if sibling.is_dir():
                potential_file = sibling / target_file
                if potential_file.is_file():
                    return potential_file
        # Move up one level in file tree.
        curr_dir = curr_dir.parent

    sys.exit(f"Could not find {target_file}.")


def main():
    parser = argparse.ArgumentParser(description="Setup query_correctness_tests environment.")
    parser.add_argument("task_ids", nargs="+")
    parser.add_argument(
        "--evergreen_bin",
        default="/home/ubuntu/evergreen",
        type=Path,
        help="Path to the evergreen executable. Defaults to /home/ubuntu/evergreen if not provided.",
    )
    parser.add_argument(
        "--out_dir",
        required=True,
        type=Path,
        help="Path to the directory to clone the query-correctness-tests repos and store evergreen log file outputs.",
    )
    parser.add_argument(
        "--repo_map",
        type=Path,
        help="Path to the test_repos.conf file. Defaults to discovering test_repos.conf by walking up the file tree starting src/mongo/db/query/query_tester/tests/scripts/setup_repro_env.py",
    )

    args = parser.parse_args()
    task_ids = args.task_ids
    evergreen = args.evergreen_bin
    out_dir = args.out_dir
    repo_map = args.repo_map if args.repo_map else discover_test_repos_conf()

    # Use task-{0,1,...,n}.log names for the output log files.
    logfiles = tuple(out_dir / f"task-{idx}.log" for idx, _ in enumerate(task_ids))
    for idx, task_id in enumerate(task_ids):
        print(f"Fetching log for {task_id}")
        fetch_evg_logs(task_id, evergreen, logfiles[idx])

    failing_tests_map = {}
    for logfile in logfiles:
        # Mutate failing_tests_map in-place.
        process_log_file(logfile, failing_tests_map)

    fetch_test_repos_dirs(
        failing_tests_map,
        create_repo_commit_map(repo_map),
        out_dir,
    )

    print(build_query_tester_command(failing_tests_map, out_dir))


if __name__ == "__main__":
    main()
