import io
import os
import subprocess
import sys

# assume we are always running from project root to find buildscripts
sys.path.append(".")

from buildscripts.install_bazel import install_bazel

bazel_bin_dir = os.path.expanduser("~/.local/bin")
if not os.path.exists(bazel_bin_dir):
    os.makedirs(bazel_bin_dir)


fake_out = io.StringIO()
orig_stdout = sys.stdout
orig_stderr = sys.stderr
sys.stdout = fake_out
sys.stderr = fake_out
bazel_executable = install_bazel(bazel_bin_dir)
sys.stdout = orig_stdout
sys.stderr = orig_stderr


cmd = (
    [
        sys.executable,
        "./buildscripts/scons.py",
    ]
    + sys.argv
    + ["BAZEL_INTEGRATION_DEBUG=1", "\\$BUILD_ROOT/scons/\\$VARIANT_DIR/sconf_temp"]
)

# Run a lightwieght scons build to generate the bazel command.
bazel_cmd_args = None
proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
for line in proc.stdout.splitlines():
    if "BAZEL_COMMAND:" in line:
        # The script is intended to be have output placed into a bash variable, so we should
        # only ever print the bazel build command
        bazel_cmd_args = line.split("BAZEL_COMMAND:")[-1].strip().split()[2:-1]
        break


# coverity requires a single target which has dependencies on all
# the cc_library and cc_binaries in our build. There is not a good way from
# within the build to get all those targets, so we will generate the list via query
# https://sig-product-docs.synopsys.com/bundle/coverity-docs/page/coverity-analysis/topics/building_with_bazel.html#build_with_bazel
proc = subprocess.run(
    [
        bazel_executable,
        "aquery",
    ]
    + bazel_cmd_args
    + [
        "--config=local",
        'mnemonic("CppCompile|LinkCompile", //src/mongo/...)',
    ],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

targets = set()
for line in proc.stdout.splitlines():
    if line.startswith("  Target: "):
        targets.add(line.split()[-1])

coverity_dir = os.path.dirname(__file__)
analysis_dir = os.path.join(coverity_dir, "analysis")
os.makedirs(analysis_dir, exist_ok=True)

with open(os.path.join(coverity_dir, "analysis", "BUILD.bazel"), "w") as buildfile:
    buildfile.write("""\
load("@rules_coverity//coverity:defs.bzl", "cov_gen_script")
cov_gen_script(
    name="coverity_build", 
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

print(
    " ".join(
        [bazel_executable, "build"] + bazel_cmd_args + ["//bazel/coverity/analysis:coverity_build"]
    )
)
