import argparse
import concurrent.futures
import glob
import json
import os
import pathlib
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description="Run tests for the IWYU analysis script.")

parser.add_argument(
    "--mongo-toolchain-bin-dir",
    type=str,
    help="Which toolchain bin directory to use for this analysis.",
    default="/opt/mongodbtoolchain/v4/bin",
)

args = parser.parse_args()

if os.getcwd() != pathlib.Path(__file__).parent:
    print(
        f"iwyu test script must run in the tests directory, changing dirs to {pathlib.Path(__file__).parent.resolve()}"
    )
    os.chdir(pathlib.Path(__file__).parent.resolve())

analysis_script = pathlib.Path(__file__).parent.parent / "run_iwyu_analysis.py"


def run_test(entry):
    print(f"Running test {pathlib.Path(entry)}...")
    test_dir = pathlib.Path(entry) / "test_run"
    if os.path.exists(test_dir):
        shutil.rmtree(test_dir)

    shutil.copytree(pathlib.Path(entry), test_dir)

    source_files = glob.glob("**/*.cpp", root_dir=test_dir, recursive=True)
    compile_commands = []

    for source_file in source_files:
        output = os.path.splitext(source_file)[0] + ".o"
        compile_commands.append(
            {
                "file": source_file,
                "command": f"{args.mongo_toolchain_bin_dir}/clang++ -o {output} -c {source_file}",
                "directory": os.path.abspath(test_dir),
                "output": output,
            }
        )

    with open(test_dir / "compile_commands.json", "w") as compdb:
        json.dump(compile_commands, compdb)

    os.makedirs(test_dir / "etc", exist_ok=True)
    with open(test_dir / "etc" / "iwyu_mapping.imp", "w") as mapping:
        mapping.write(
            '[{include: ["\\"placeholder.h\\"", "private", "\\"placeholder2.h\\"", "public"]}]'
        )

    iwyu_run = subprocess.run(
        [sys.executable, analysis_script, "--verbose", "--config-file=test_config.yml"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=test_dir,
    )

    results_run = subprocess.run(
        [sys.executable, pathlib.Path(entry) / "expected_results.py"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        cwd=test_dir,
    )

    msg = "\n".join([iwyu_run.stdout, results_run.stdout, f"FAILED!: {pathlib.Path(entry)}"])
    msg = "\n".join([f"[{pathlib.Path(entry).name}] {line}" for line in msg.split("\n")])

    if results_run.returncode != 0:
        return results_run.returncode, msg, pathlib.Path(entry).name
    else:
        return (
            results_run.returncode,
            f"[{pathlib.Path(entry).name}] PASSED!: {pathlib.Path(entry)}",
            pathlib.Path(entry).name,
        )


failed_tests = []
with concurrent.futures.ThreadPoolExecutor(
    max_workers=len(os.sched_getaffinity(0)) + 4
) as executor:
    # create and run the IWYU jobs
    future_cmd = {
        executor.submit(run_test, entry): entry
        for entry in pathlib.Path(__file__).parent.glob("*")
        if os.path.isdir(entry)
    }

    # process the results
    for future in concurrent.futures.as_completed(future_cmd):
        result, message, test_name = future.result()
        if result != 0:
            failed_tests += [test_name]
        print(message)

print("\n***Tests complete.***")
if failed_tests:
    print("The following tests failed:")
    for test in failed_tests:
        print(" - " + test)
    print("Please review the logs above for more information.")
