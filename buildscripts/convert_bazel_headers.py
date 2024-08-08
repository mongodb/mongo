import subprocess
import sys
import json
import platform
import os
import shlex
import concurrent.futures
import glob
import traceback
import shutil
from typing import Annotated

import typer


def main(
    target_library: Annotated[str, typer.Option()], silent: Annotated[bool, typer.Option()] = False
):
    extra_args = []
    if os.name == "nt":
        extra_args += [
            "CPPPATH=C:\sasl\include",
            "LIBPATH=C:\sasl\lib",
        ]
        target_library = os.path.join(
            os.path.dirname(target_library), os.path.basename(target_library)[3:-2] + "lib"
        )

    if platform.system() == "Darwin":
        target_library = target_library[:-2] + "a"

    path = shutil.which("icecc")
    if path is None:
        extra_args += ["ICECC="]

    cmd = [
        sys.executable,
        "buildscripts/scons.py",
        "--build-profile=opt",
        f"--bazel-includes-info={target_library}",
        "--libdeps-linting=off",
        "--ninja=disabled",
        "compiledb",
    ] + extra_args

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    while True:
        line = p.stdout.readline()
        if not line:
            break
        if not silent:
            print(line.strip())

    _, _ = p.communicate()

    if p.returncode != 0:
        print(f"SCons build failed, exit code {p.returncode}", file=sys.stderr)
        sys.exit(1)

    with open("compile_commands.json") as f:
        cc = json.load(f)

    headers = set()
    original_headers = set()

    def get_headers(line):
        try:
            with open(target_library + ".bazel_headers") as f:
                bazel_headers = [line.strip() for line in f.readlines()]
                bazel_headers += [
                    "src/mongo/platform/basic.h",
                    "src/mongo/platform/windows_basic.h",
                ]

            with open(target_library + ".env_vars") as f:
                tmp_env_vars = json.load(f)
                env_vars = {}
                # subprocess requies only strings
                for k, v in tmp_env_vars.items():
                    env_vars[str(k)] = str(v)

            for command in cc:
                cmd_output = command["output"].replace("\\", "/").strip("'").strip('"')
                line_output = line.replace("\\", "/")

                if cmd_output == line_output:
                    os.makedirs(os.path.dirname(line), exist_ok=True)
                    if os.name == "nt":
                        header_arg = " /showIncludes"
                    else:
                        header_arg = " -H"

                    if not silent:
                        print(f"compiling {line}")
                        print(command["command"] + header_arg)

                    p = subprocess.run(
                        shlex.split((command["command"].replace("\\", "/") + header_arg)),
                        env=env_vars,
                        capture_output=True,
                        text=True,
                    )
                    if p.returncode != 0:
                        print(f"Error compiling, exitcode: {p.returncode}", file=sys.stderr)
                        print(f"STDOUT: {p.stdout}", file=sys.stderr)
                        print(f"STDERR: {p.stderr}", file=sys.stderr)
                        sys.exit(1)
                    if os.name == "nt":
                        for line in p.stdout.split("\n"):
                            line = (
                                line.replace("Note: including file:", "")
                                .strip(" ")
                                .replace("\\", "/")
                            )

                            if not line.startswith(os.getcwd().replace("\\", "/")):
                                continue

                            line = os.path.relpath(
                                line, start=os.getcwd().replace("\\", "/")
                            ).replace("\\", "/")
                            if line not in bazel_headers:
                                if line.startswith("src/"):
                                    original_headers.add(line)
                                    line = "//" + line
                                    line = ":".join(line.rsplit("/", 1))

                                    headers.add(line)
                    else:
                        for line in p.stderr.split("\n"):
                            if ". src/" in line:
                                while line.startswith("."):
                                    line = line[1:]
                                line = line.replace("\\", "/")

                                if line[1:] not in bazel_headers:
                                    original_headers.add(line[1:])
                                    line = "//" + line[1:]
                                    line = ":".join(line.rsplit("/", 1))

                                    headers.add(line)
        except Exception as exc:
            print(traceback.format_exc(), file=sys.stderr)
            raise exc

    with open(target_library + ".obj_files") as f:
        if platform.system() == "Linux":
            cpu_count = len(os.sched_getaffinity(0)) + 4
        else:
            cpu_count = os.cpu_count() + 4

        with concurrent.futures.ThreadPoolExecutor(max_workers=cpu_count) as executor:
            jobs = {
                executor.submit(get_headers, line.strip()): line.strip() for line in f.readlines()
            }
            for completed_job in concurrent.futures.as_completed(jobs):
                if not silent:
                    print(f"finished {jobs[completed_job]}")

    uniq_dirs = dict()
    for header in original_headers:
        dir_name = os.path.dirname(header)
        if dir_name not in uniq_dirs:
            uniq_dirs[dir_name] = []
        uniq_dirs[dir_name].append(header)

    print(f"header list for {target_library}")
    print("  header utilization per directory:")
    for uniq_dir in sorted(uniq_dirs):
        total_headers = (
            glob.glob(os.path.join(uniq_dir, "*.h"))
            + glob.glob(os.path.join(uniq_dir, "*.ipp"))
            + glob.glob(os.path.join(uniq_dir, "*.hpp"))
        )
        if len(total_headers) != 0:
            print(
                f"    dir: {uniq_dir}, utilization: {len(uniq_dirs[uniq_dir])/len(total_headers):.2%}"
            )
        else:
            print(
                f"found no headers in dir {uniq_dir}, but had headers listed: {uniq_dirs[uniq_dir]}"
            )
    print("  header list:")
    for header in sorted(headers):
        print(f'    "{header}",')


if __name__ == "__main__":
    typer.run(main)
