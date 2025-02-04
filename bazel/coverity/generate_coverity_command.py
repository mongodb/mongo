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
proc = subprocess.run(
    [
        bazel_executable,
        bazel_cache,
        "aquery",
    ]
    + bazel_cmd_args
    + [args.bazel_query],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

targets = set()
for line in proc.stdout.splitlines():
    if line.startswith("  Target: "):
        targets.add(line.split()[-1])


enterprise_coverity_dir = os.path.join("src", "mongo", "db", "modules", "enterprise", "coverity")
os.makedirs(enterprise_coverity_dir, exist_ok=True)
with open(os.path.join(enterprise_coverity_dir, "BUILD.bazel"), "w") as buildfile:
    buildfile.write("""\
load("@rules_coverity//coverity:defs.bzl", "cov_gen_script")
cov_gen_script(
    name="enterprise_coverity_build",
    testonly=True,
    tags=["coverity"],
    deps=[
""")
    for target in targets:
        buildfile.write(
            """\
        "%s",
"""
            % target
        )

    buildfile.write("""\
    ],
)
""")
