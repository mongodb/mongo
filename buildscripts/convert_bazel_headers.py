import concurrent.futures
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import traceback
from typing import Annotated, List

import typer


def work(target_library: str, silent: bool, cpu_count: int, cc: List[str]):
    headers = set()
    original_headers = set()

    def get_headers(line):
        nonlocal headers
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
            src_header = os.path.splitext(line[2:])[0] + ".h"
            if os.path.exists(src_header):
                src_header = "//" + ":".join(src_header.rsplit("/", 1))
                headers.add(src_header)
            line = ":".join(line.rsplit("/", 1))
            if line.endswith("_gen.cpp"):
                line = line[:-4]
            sources.append(line)

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

    basename_sources = [os.path.splitext(src.rsplit(":", 1)[1])[0] for src in sources]
    for header in headers:
        header_basename = os.path.splitext(header.rsplit(":", 1)[1])[0]
        if header_basename in basename_sources:
            minimal_headers.add(header)
            continue

        if header in reverse_header_map:
            found = False
            for lib in reverse_header_map[header]:
                recommended_deps.add(lib)
        else:
            if not header.endswith(global_headers):
                minimal_headers.add(header)

    deps_order_by_height = []
    deps_queries = {}

    with open(target_library + ".bazel_deps") as f:
        original_deps = [line.strip() for line in f.readlines()]

    for dep in recommended_deps | set(original_deps):
        p = subprocess.run(
            [bazel_exec, "cquery"]
            + bazel_config
            + [f'kind("extract_debuginfo|idl_generator|render_template", deps("@{dep}"))'],
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
            if dep in header_map and path_header in header_map[dep]:
                optimal_header_deps.add(dep)
                break
            found = False
            for other_dep in deps_order_by_height:
                if other_dep in gen_header_map:
                    continue
                if dep in deps_queries[other_dep]:
                    optimal_header_deps.add(other_dep)
                    found = True
                    break
            if found:
                continue
            if dep in gen_header_map:
                minimal_headers.add(dep)
            else:
                raise Exception(
                    f"Should not happen, did not find way to add dep {dep} for {target_library}"
                )

    optimal_header_deps = list(optimal_header_deps)

    working_deps = optimal_header_deps.copy()
    for dep in optimal_header_deps:
        if dep in working_deps:
            for test_dep in optimal_header_deps:
                if test_dep == dep:
                    continue
                if test_dep in working_deps and test_dep in deps_queries[dep]:
                    working_deps.remove(test_dep)

    link_deps = []
    header_deps = []
    for dep in sorted(list(set(list(working_deps) + list(set(original_deps))))):
        if dep in original_deps:
            link_deps.append(dep)
        else:
            header_deps.append(dep)

    target_name = os.path.splitext(os.path.basename(target_library))[0]
    if target_name.startswith("lib"):
        target_name = target_name[3:]

    bazel_target = f"{target_library}\n"
    bazel_target += "=" * 50 + "\n"
    local_bazel_path = os.path.dirname(target_library.replace("build/opt", "//src")) + ":"
    bazel_target += "mongo_cc_library(\n"
    bazel_target += f'    name = "{target_name}",\n'
    if sources:
        bazel_target += "    srcs = [\n"
        for src in sorted([src.replace(local_bazel_path, "") for src in sources]):
            bazel_target += f'        "{src}",\n'
        bazel_target += "    ],\n"
    if minimal_headers:
        bazel_target += "    hdrs = [\n"
        for header in sorted([header.replace(local_bazel_path, "") for header in minimal_headers]):
            bazel_target += f'        "{header}",\n'
        bazel_target += "    ],\n"
    if header_deps:
        bazel_target += "    header_deps = [\n"
        for dep in sorted([dep.strip().replace(local_bazel_path, "") for dep in header_deps]):
            bazel_target += f'        "{dep}",\n'
        bazel_target += "    ],\n"
    if link_deps:
        bazel_target += "    deps = [\n"
        for dep in sorted([dep.strip().replace(local_bazel_path, "") for dep in link_deps]):
            bazel_target += f'        "{dep}",\n'
        bazel_target += "    ],\n"
    bazel_target += ")\n"
    return bazel_target


def main(
    target_libraries: Annotated[List[str], typer.Argument()],
    silent: Annotated[bool, typer.Option()] = False,
    skip_scons: Annotated[bool, typer.Option()] = False,
    debug_mode: Annotated[bool, typer.Option()] = False,
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

    path = shutil.which("icecc")
    if path is None:
        extra_args += ["ICECC="]

    # Define separate functions instead of using lambdas
    def target_fmt_nt(target_library: str) -> str:
        return os.path.join(
            os.path.dirname(target_library), os.path.basename(target_library)[3:-2] + "lib"
        )

    def target_fmt_darwin(target_library: str) -> str:
        return target_library[:-2] + "a"

    def target_fmt_default(x: str) -> None:
        return None

    if os.name == "nt":
        target_fmt = target_fmt_nt
    elif platform.system() == "Darwin":
        target_fmt = target_fmt_darwin
    else:
        target_fmt = target_fmt_default

    map(target_fmt, target_libraries)

    cmd = [
        sys.executable,
        "buildscripts/scons.py",
        "--build-profile=opt",
        " ".join(
            [f"--bazel-includes-info={target_library}" for target_library in target_libraries]
        ),
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
    if platform.system() == "Linux":
        cpu_count = len(os.sched_getaffinity(0)) + 4
    else:
        cpu_count = os.cpu_count() + 4

    # Process pool makes it harder to debug what is happening
    # so for debug mode, we disabled process pool so things happen in order
    # you can just print from the process.
    if debug_mode:
        bazel_targets = []
        for target_library in target_libraries:
            bazel_targets += [work(target_library, silent, cpu_count, cc)]
    else:
        with concurrent.futures.ProcessPoolExecutor(max_workers=cpu_count) as executor:
            jobs = {
                executor.submit(work, target_library, silent, cpu_count, cc): target_library
                for target_library in target_libraries
            }
            bazel_targets = [job.result() for job in concurrent.futures.as_completed(jobs)]

    print("====== Bazel Targets ======\n")
    print("\n".join(bazel_targets))


if __name__ == "__main__":
    typer.run(main)
