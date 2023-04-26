import os
import sys
import shutil
import subprocess
import glob
from concurrent import futures
from pathlib import Path
import time
from typing import List, Tuple

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

# pylint: disable=wrong-import-position
from buildscripts.linter.filediff import gather_changed_files_for_lint
from buildscripts import simple_report

# pylint: enable=wrong-import-position


def is_js_file(filename: str) -> bool:
    # return True
    return filename.startswith("jstests") and filename.endswith(".js")


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

OUTPUT_FULL_DIR = Path(os.getcwd()) / OUTPUT_DIR
INPUT_FULL_DIR = Path(os.getcwd()) / INPUT_DIR

subprocess.run([
    "./src/scripts/npm_run.sh", "jstestfuzz", "--", "--jsTestsDir", INPUT_FULL_DIR, "--out",
    OUTPUT_FULL_DIR, "--numSourceFiles",
    str(min(num_changed_files, 250)), "--numGeneratedFiles", "250"
], check=True, cwd="jstestfuzz")


def _parse_jsfile(jsfile: Path) -> simple_report.Result:
    """
    Takes in a path to be attempted to parse
    Returns what should be added to the report given to evergreen
    """
    print(f"Trying to parse jsfile {jsfile}")
    start_time = time.time()
    proc = subprocess.run(["./src/scripts/npm_run.sh", "parse-jsfiles", "--",
                           str(jsfile)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          cwd="jstestfuzz")
    end_time = time.time()
    status = "pass" if proc.returncode == 0 else "fail"
    npm_run_output = proc.stdout.decode("UTF-8")
    if proc.returncode == 0:
        print(f"Successfully to parsed jsfile {jsfile}")
    else:
        print(f"Failed to parsed jsfile {jsfile}")
        print(npm_run_output)
    return simple_report.Result(status=status, exit_code=proc.returncode, start=start_time,
                                end=end_time, test_file=jsfile.name, log_raw=npm_run_output)


report = simple_report.Report(failures=0, results=[])

with futures.ThreadPoolExecutor() as executor:
    parse_jsfiles_futures = [
        executor.submit(_parse_jsfile, Path(jsfile))
        for jsfile in glob.iglob(str(OUTPUT_FULL_DIR / "**"), recursive=True)
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
