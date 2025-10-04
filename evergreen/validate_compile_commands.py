import json
import os
import subprocess
import sys

default_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
if not default_dir:
    print(
        "This script must be run though bazel. Please run 'bazel run //evergreen:validate_compile_commands' instead."
    )
    sys.exit(1)

os.chdir(default_dir)

if not os.path.exists("compile_commands.json"):
    sys.stderr.write("The 'compile_commands.json' file was not found.\n")
    sys.stderr.write("Attempting to run 'bazel build compiledb' to generate it.\n")
    subprocess.run(["bazel", "build", "compiledb"], check=True)

with open("compile_commands.json") as f:
    compiledb = json.load(f)
    # super basic check for now
    if len(compiledb) < 1000:
        sys.stderr.write(
            f"ERROR: 'compile_commands.json' has less than 1000 entries. Found {len(compiledb)} entries.\n"
        )
        sys.exit(1)
    print("Successfully validated compile_commands.json file.")
