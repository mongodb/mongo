import glob
import os
import shutil
import subprocess
import sys
import time
from concurrent import futures
from pathlib import Path

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts import simple_report
from buildscripts.linter.filediff import gather_changed_files_for_lint


def is_js_file(filename: str) -> bool:
    # return True
    return (
        filename.startswith("jstests")
        or filename.startswith("src/mongo/db/modules/enterprise/jstests")
    ) and filename.endswith(".js")


diffed_files = [Path(f) for f in gather_changed_files_for_lint(is_js_file)]
num_changed_files = len(diffed_files)

if num_changed_files == 0:
    print("No js files had changes in them. Exiting.")
    sys.exit(0)

INPUT_DIR = "jstestfuzzinput"
OUTPUT_DIR = "jstestfuzzoutput"
os.makedirs(INPUT_DIR, exist_ok=True)
os.makedirs(OUTPUT_DIR, exist_ok=True)

for file in diffed_files:
    copy_dest = INPUT_DIR / file
    os.makedirs(copy_dest.parent, exist_ok=True)
    shutil.copy(file, copy_dest)

CONTAINER_RUNTIME = os.environ.get("CONTAINER_RUNTIME")
LOCAL_OUTPUT_FULL_DIR = Path(os.getcwd()) / OUTPUT_DIR
LOCAL_INPUT_FULL_DIR = Path(os.getcwd()) / INPUT_DIR

CONTAINER_INPUT_PATH = f"/app/{INPUT_DIR}"
CONTAINER_OUTPUT_PATH = f"/app/{OUTPUT_DIR}"

subprocess.run(
    [
        CONTAINER_RUNTIME,
        "run",
        "--rm",
        "-v",
        f"{LOCAL_INPUT_FULL_DIR}:{CONTAINER_INPUT_PATH}",
        "-v",
        f"{LOCAL_OUTPUT_FULL_DIR}:{CONTAINER_OUTPUT_PATH}",
        "901841024863.dkr.ecr.us-east-1.amazonaws.com/mongodb-internal/jstestfuzz:latest",
        "npm",
        "run-script",
        "jstestfuzz",
        "--",
        "--jsTestsDir",
        CONTAINER_INPUT_PATH,
        "--out",
        CONTAINER_OUTPUT_PATH,
        "--numSourceFiles",
        str(min(num_changed_files, 100)),
        "--numGeneratedFiles",
        "250",
        # This parameter is used to limit the output file size to avoid timeouts when running them.
        # For this task, we just want to sanity check that we *can* generate the files so loosen
        # the restriction.
        "--maxFileSizeMB",
        "10",
    ],
    check=True,
)


def _parse_jsfile(jsfile: Path) -> simple_report.Result:
    """
    Takes in a path to be attempted to parse.

    Returns what should be added to the report given to evergreen
    """
    # Find the relative path to the jsfile in the volume on the container
    relative_js_file_path = f"{INPUT_DIR}/{jsfile.relative_to(LOCAL_INPUT_FULL_DIR)}"
    start_time = time.time()
    proc = subprocess.run(
        [
            CONTAINER_RUNTIME,
            "run",
            "--rm",
            "-v",
            f"{LOCAL_INPUT_FULL_DIR}:{CONTAINER_INPUT_PATH}",
            "-v",
            f"{LOCAL_OUTPUT_FULL_DIR}:{CONTAINER_OUTPUT_PATH}",
            "901841024863.dkr.ecr.us-east-1.amazonaws.com/mongodb-internal/jstestfuzz:latest",
            "npm",
            "run-script",
            "parse-jsfiles",
            "--",
            str(relative_js_file_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    end_time = time.time()
    status = "pass" if proc.returncode == 0 else "fail"
    npm_run_output = proc.stdout.decode("UTF-8")
    if proc.returncode == 0:
        print(f"Successfully to parsed jsfile {jsfile}")
    else:
        print(f"Failed to parsed jsfile {jsfile}")
        print(npm_run_output)
    return simple_report.Result(
        status=status,
        exit_code=proc.returncode,
        start=start_time,
        end=end_time,
        test_file=jsfile.name,
        log_raw=npm_run_output,
    )


report = simple_report.Report(failures=0, results=[])

with futures.ThreadPoolExecutor() as executor:
    parse_jsfiles_futures = [
        executor.submit(_parse_jsfile, Path(jsfile))
        for jsfile in glob.iglob(f"{LOCAL_INPUT_FULL_DIR}/**", recursive=True)
        if os.path.isfile(jsfile)
    ]

    for future in futures.as_completed(parse_jsfiles_futures):
        result = future.result()
        report["results"].append(result)
        report["failures"] += 1 if result["exit_code"] != 0 else 0

simple_report.put_report(report)
if report["failures"] > 0:
    print("Had at least one failure, exiting with 1")
    sys.exit(1)

print("No failures, exiting success")
sys.exit(0)
