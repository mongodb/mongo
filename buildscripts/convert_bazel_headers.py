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
    target_library: Annotated[str, typer.Option()],
    silent: Annotated[bool, typer.Option()] = False,
    skip_scons: Annotated[bool, typer.Option()] = False,
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

    if not skip_scons:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        while True:
            line = p.stdout.readline()
            if not line:
                break
            print(line.strip(), file=sys.stderr)

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
                                if line.startswith("src/") or line.startswith("bazel-out/"):
                                    original_headers.add(line)
                                    line = "//" + line
                                    line = ":".join(line.rsplit("/", 1))

                                    headers.add(line)
                    else:
                        for line in p.stderr.split("\n"):
                            if ". src/" in line or ". bazel-out/" in line:
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

    sources = []
    with open(target_library + ".obj_files") as f:
        lines = f.readlines()
        for line in lines:
            line = line.strip()
            line = line.replace("build/opt", "//src")
            line = line[: line.find(".")] + ".cpp"
            line = ":".join(line.rsplit("/", 1))
            if line.endswith("_gen.cpp"):
                line = line[:-4]
            sources.append(line)
        if platform.system() == "Linux":
            cpu_count = len(os.sched_getaffinity(0)) + 4
        else:
            cpu_count = os.cpu_count() + 4

        with concurrent.futures.ThreadPoolExecutor(max_workers=cpu_count) as executor:
            jobs = {executor.submit(get_headers, line.strip()): line.strip() for line in lines}
            for completed_job in concurrent.futures.as_completed(jobs):
                if not silent:
                    print(f"finished {jobs[completed_job]}")

    with open(".bazel_include_info.json") as f:
        bazel_include_info = json.load(f)

    header_map = bazel_include_info["header_map"]
    gen_header_map = bazel_include_info["gen_header_map"]
    bazel_exec = bazel_include_info["bazel_exec"]
    bazel_config = bazel_include_info["config"]

    global_headers = (
        "src/mongo:config.h",
        "src/mongo/config.h",
        "src/mongo/platform/basic.h",
        "src/mongo/platform/windows_basic.h",
    )

    reverse_header_map = {}
    for k, v in header_map.items():
        for hdr in v:
            if not hdr or hdr.endswith(global_headers):
                continue
            bazel_header = "//" + hdr.replace("\\", "/")
            bazel_header = ":".join(bazel_header.rsplit("/", 1))
            if bazel_header.startswith("//src/third_party/SafeInt"):
                reverse_header_map[bazel_header] = ["//src/third_party/SafeInt:headers"]
            elif bazel_header.startswith("//src/third_party/immer"):
                reverse_header_map[bazel_header] = ["//src/third_party/immer:headers"]
            elif bazel_header in reverse_header_map:
                if bazel_header.startswith("//src/third_party/"):
                    continue
                reverse_header_map[bazel_header].append(k)
            else:
                reverse_header_map[bazel_header] = [k]

    for k, v in gen_header_map.items():
        for hdr in v:
            if not hdr or hdr.endswith(global_headers):
                continue
            bazel_header = "//" + hdr.replace("\\", "/")
            bazel_header = ":".join(bazel_header.rsplit("/", 1))
            if bazel_header not in reverse_header_map:
                reverse_header_map[bazel_header] = [k]
            else:
                reverse_header_map[bazel_header].append(k)

    recommended_deps = set()
    minimal_headers = set()
    for header in headers:
        if header in reverse_header_map:
            found = False
            for lib in reverse_header_map[header]:
                if lib in gen_header_map:
                    minimal_headers.add(lib)
                    found = True
                    break
            if not found:
                for lib in reverse_header_map[header]:
                    recommended_deps.add(lib)
        else:
            if not header.endswith(global_headers):
                minimal_headers.add(header)

    deps_order_by_height = []
    deps_queries = {}
    for dep in recommended_deps:
        p = subprocess.run(
            [bazel_exec, "cquery"] + bazel_config + [f'kind("extract_debuginfo", deps("@{dep}"))'],
            capture_output=True,
            text=True,
        )
        deps_queries[dep] = [
            line.split(" ")[0] for line in p.stdout.splitlines() if line.startswith("//")
        ]
        deps_order_by_height.append((dep, len(deps_queries[dep])))

    deps_order_by_height.sort(key=lambda x: x[1])

    deps_order_by_height = [dep[0] for dep in deps_order_by_height]
    optimal_header_deps = set()
    for header in headers:
        if header in minimal_headers:
            continue

        path_header = "/".join(header.rsplit(":", 1))
        path_header = path_header[2:]
        for dep in deps_order_by_height:
            if path_header in header_map[dep]:
                optimal_header_deps.add(dep)
                break
    optimal_header_deps = list(optimal_header_deps)

    working_deps = optimal_header_deps.copy()
    for dep in optimal_header_deps:
        if dep in working_deps:
            for test_dep in optimal_header_deps:
                if test_dep == dep:
                    continue
                if test_dep in working_deps and test_dep in deps_queries[dep]:
                    working_deps.remove(test_dep)

    with open(target_library + ".bazel_deps") as f:
        original_deps = f.readlines()

    link_deps = []
    header_deps = []
    for dep in sorted(list(working_deps) + list(set(original_deps))):
        if dep in original_deps:
            link_deps.append(dep)
        else:
            header_deps.append(dep)

    target_name = os.path.splitext(os.path.basename(target_library))[0]
    if target_name.startswith("lib"):
        target_name = target_name[3:]

    local_bazel_path = os.path.dirname(target_library.replace("build/opt", "//src")) + ":"
    print("mongo_cc_library(")
    print(f'    name = "{target_name}",')
    if sources:
        print(f"    srcs = [")
        for src in sources:
            print(f'        "{src.replace(local_bazel_path, "")}",')
        print("    ],")
    if minimal_headers:
        print("    hdrs = [")
        for header in sorted(minimal_headers):
            print(f'        "{header.replace(local_bazel_path, "")}",')
        print("    ],")
    if header_deps:
        print("    header_deps = [")
        for dep in sorted(header_deps):
            print(f'        "{dep.strip().replace(local_bazel_path, "")}",')
        print("    ],")
    if link_deps:
        print("    deps = [")
        for dep in sorted(link_deps):
            print(f'        "{dep.strip().replace(local_bazel_path, "")}",')
        print("    ],")
    print(")")


if __name__ == "__main__":
    typer.run(main)
