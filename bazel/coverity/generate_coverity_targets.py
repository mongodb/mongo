import argparse
import os
import subprocess

parser = argparse.ArgumentParser(description="Generate coverity build file.")
parser.add_argument("--bazel_executable", required=True)
parser.add_argument("--bazel_cache", required=True)
parser.add_argument("--bazel_query", required=True)

args, unknownargs = parser.parse_known_args()

bazel_cmd_args = unknownargs
bazel_executable = os.path.expanduser(args.bazel_executable)
bazel_cache = os.path.expanduser(args.bazel_cache)

# coverity requires a single target which has dependencies on all
# the cc_library and cc_binaries in our build. There is not a good way from
# within the build to get all those targets, so we will generate the list via query
# https://sig-product-docs.synopsys.com/bundle/coverity-docs/page/coverity-analysis/topics/building_with_bazel.html#build_with_bazel
cmd = [
        bazel_executable,
        bazel_cache,
        "aquery",
    ] + bazel_cmd_args + [args.bazel_query]
print(f"Running command: {cmd}")
proc = subprocess.run(
    cmd,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

print(proc.stderr)

targets = set()
with open('coverity_targets.list', 'w') as f: 
    for line in proc.stdout.splitlines():
        if line.startswith("  Target: "):
            f.write(line.split()[-1] + "\n")


