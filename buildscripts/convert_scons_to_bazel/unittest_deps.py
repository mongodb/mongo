import argparse
import glob
import os
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--full-list", action="store_true")
parser.add_argument("--check-test-deps")

args = parser.parse_args()

if not os.path.exists("opt.ninja"):
    subprocess.run(["python", "buildscripts/scons.py", "--build-profile=opt"])

print("Gathering unittests...")
all_ut_proc = subprocess.run(["ninja", "-f", "opt.ninja", "-t", "inputs", "install-unittests"],
                             capture_output=True, text=True)
print("done.")
ut_deps = {}
for ut_line in all_ut_proc.stdout.split("\n"):
    if not args.full_list and not ut_line.endswith("/" + args.check_test_deps):
        continue
    if ut_line.endswith("_test") and not ut_line.startswith('bazel-bin/install/'):
        print(f"Checking deps for {ut_line}")
        ut_proc = subprocess.run(["ninja", "-f", "opt.ninja", "-t", "query", ut_line],
                                 capture_output=True, text=True)
        deps = re.findall(r"\s(build\/opt\/[^\s]*?\.so).", ut_proc.stdout, re.DOTALL)
        ut_deps[ut_line] = [dep for dep in deps if "/libshim" not in dep]

if args.full_list:
    print("Ordered List:")
    for k, v in sorted(ut_deps.items(), key=lambda tup: len(tup[1])):
        print(f"{k}: {len(v)}")
else:
    assert (len(ut_deps) == 1)
    deps_not_converted = None
    for ut, deps in ut_deps.items():
        deps_not_converted = deps.copy()
    for bazel_file in glob.glob("**/BUILD.bazel", recursive=True):
        deps_to_remove = set()
        with open(bazel_file) as f:
            content = f.read()
            for dep in deps_not_converted:
                base_target = dep[dep.rfind("/"):][4:-3]
                if 'name = "' + base_target + '"' in content:
                    deps_to_remove.add(dep)
            for dep in deps_to_remove:
                deps_not_converted.remove(dep)
    print("Deps left to convert:")
    for dep in deps_not_converted:
        print(dep)
