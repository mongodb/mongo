import hashlib
import json
import os
import sys
from functools import partial
from pathlib import Path

import libdeps_tool
import SCons


def exists(env):
    return True


def get_md5(file_path):
    h = hashlib.md5()

    with open(file_path, "rb") as file:
        while True:
            # Reading is buffered, so we can read smaller chunks.
            chunk = file.read(h.block_size)
            if not chunk:
                break
            h.update(chunk)

    return h.hexdigest()


def get_target_headers(env, target, header_query, symlink_query=None):
    header_list_cache_dir = Path(".bazel_header_list_cache")
    target_path = "/".join(target.rsplit(":", 1))[2:]
    bazel_file = Path(os.path.dirname(target_path)) / "BUILD.bazel"
    cache_file = str(header_list_cache_dir / bazel_file) + ".json"
    build_file_hash = get_md5(bazel_file)
    cache_data = None

    if os.path.exists(cache_file):
        with open(cache_file) as f:
            cache_data = json.load(f)
            if cache_data["MD5"] == build_file_hash:
                if target in cache_data:
                    return cache_data[target]["headers"], cache_data[target]["macro_name"]
            else:
                # invalidate the cache
                cache_data = None

    if cache_data is None:
        print(f"{bazel_file} changed, invalidating cache")
        os.makedirs(os.path.dirname(cache_file), exist_ok=True)
        cache_data = dict()
        cache_data["MD5"] = build_file_hash

    print(f"getting {target} headers")
    results = env.RunBazelQuery(header_query, f"getting {target} headers")
    cache_data[target] = {"headers": [], "macro_name": target}
    for line in results.stdout.split("\n"):
        cache_data[target]["headers"] += [line]

    if symlink_query is not None:
        target_results = env.RunBazelQuery(symlink_query, f"getting macro name for {target}")
        cache_data[target]["macro_name"] = target_results.stdout.split(" ")[0]

    with open(cache_file, "w") as f:
        json.dump(cache_data, f)

    return cache_data[target]["headers"], cache_data[target]["macro_name"]


def add_headers_from_all_libraries(env, header_map):
    bazel_query = ["aquery"] + env["BAZEL_FLAGS_STR"] + ['mnemonic("CppArchive", //src/...)']
    results = env.RunBazelQuery(bazel_query, "getting all bazel libraries")
    targets = set()
    for line in results.stdout.split("\n"):
        if "  Target: //src" in line:
            target = line.split("  Target: ")[-1]
            targets.add(target)

    for target in targets:
        header_query = (
            ["cquery"]
            + env["BAZEL_FLAGS_STR"]
            + [
                f'labels(hdrs, "@{target}")',
                "--output",
                "files",
            ]
        )
        macro_name_query = (
            ["cquery"]
            + env["BAZEL_FLAGS_STR"]
            + [
                f'kind("extract_debuginfo", rdeps(@//src/mongo/..., "@{target}",  1))',
            ]
        )
        headers, macro_name = get_target_headers(env, target, header_query, macro_name_query)
        header_map[macro_name] = [hdr for hdr in headers if not hdr.endswith("src/mongo/config.h")]


def add_headers_from_gen_code(env, header_map):
    source_generators_query = (
        ["aquery"]
        + env["BAZEL_FLAGS_STR"]
        + ['mnemonic("IdlcGenerator|TemplateRenderer|ConfigHeaderGen", //src/...)']
    )

    idl_gen_targets = set()
    results = env.RunBazelQuery(source_generators_query, "getting all idl gen targets")
    for line in results.stdout.split("\n"):
        if "  Target: //src" in line:
            target = line.split("  Target: ")[-1]
            idl_gen_targets.add(target)

    for target in idl_gen_targets:
        header_query = (
            ["cquery"]
            + env["BAZEL_FLAGS_STR"]
            + [
                f"@{target}",
                "--output",
                "files",
            ]
        )
        headers, macro_name = get_target_headers(env, target, header_query)
        header_map[macro_name] = [
            hdr for hdr in headers if hdr.endswith(target.split(":")[-1] + ".h")
        ]

    source_generators_query = (
        ["aquery"]
        + env["BAZEL_FLAGS_STR"]
        + ['mnemonic("TemplateRenderer|ConfigHeaderGen", //src/...)']
    )

    source_gen_targets = set()
    results = env.RunBazelQuery(source_generators_query, "getting all source gen targets")
    for line in results.stdout.split("\n"):
        if "  Target: //src" in line:
            target = line.split("  Target: ")[-1]
            source_gen_targets.add(target)

    for target in source_gen_targets:
        header_query = (
            ["cquery"]
            + env["BAZEL_FLAGS_STR"]
            + [
                f"@{target}",
                "--output",
                "files",
            ]
        )
        headers, macro_name = get_target_headers(env, target, header_query)
        header_map[macro_name] = [
            hdr for hdr in headers if hdr.endswith(".h") and not hdr.endswith("src/mongo/config.h")
        ]


def bazel_includes_emitter(target_libraries, target, source, env):
    rel_target = os.path.relpath(str(target[0].abspath), start=env.Dir("#").abspath).replace(
        "\\", "/"
    )

    if rel_target in target_libraries:
        objsuffix = (
            env.subst("$OBJSUFFIX") if not env.TargetOSIs("linux") else env.subst("$SHOBJSUFFIX")
        )
        builder_name = "StaticLibrary" if not env.TargetOSIs("linux") == "nt" else "SharedLibrary"
        os.makedirs(os.path.dirname(str(target[0].abspath)), exist_ok=True)
        with open(str(target[0].abspath) + ".obj_files", "w") as f:
            for s in source:
                if str(s).endswith(objsuffix):
                    f.write(os.path.relpath(str(s.abspath), start=env.Dir("#").abspath) + "\n")
        with open(str(target[0].abspath) + ".env_vars", "w") as f:
            json.dump(env["ENV"], f)

        with (
            open(str(target[0].abspath) + ".bazel_headers", "w") as fheaders,
            open(str(target[0].abspath) + ".bazel_deps", "w") as fdeps,
        ):
            # note we can't know about LIBDEPS_DEPDENDENTS (reverse deps) in an emitter
            # however we do co-opt the libdeps linter to check for these at the end of reading
            # sconscripts

            deps = []
            for s in (
                env.get("LIBDEPS", [])
                + env.get("LIBDEPS_PRIVATE", [])
                + env.get("LIBDEPS_INTERFACE", [])
            ):
                if not s:
                    continue

                libnode = libdeps_tool._get_node_with_ixes(env, s, builder_name)

                libnode_path = os.path.relpath(
                    str(libnode.abspath), start=env.Dir("#").abspath
                ).replace("\\", "/")
                if libnode.has_builder() and libnode.get_builder().get_name(env) != "ThinTarget":
                    print(
                        f"ERROR: can generate correct bazel header list because {target[0]} has non-bazel dependency: {libnode}"
                    )
                    sys.exit(1)
                if str(libnode_path) in env["SCONS2BAZEL_TARGETS"].scons2bazel_targets:
                    bazel_target = env["SCONS2BAZEL_TARGETS"].bazel_target(str(libnode_path))
                    # new query to run, run and cache it
                    deps.append(bazel_target)
                    bazel_query = (
                        ["cquery"]
                        + env["BAZEL_FLAGS_STR"]
                        + [
                            f'filter("[\\.h,\\.ipp,\\.hpp].*$", kind("source", deps("@{bazel_target}")))',
                            "--output",
                            "files",
                        ]
                    )
                    results = env.RunBazelQuery(bazel_query, "getting bazel headers")

                    if results.returncode != 0:
                        print("ERROR: bazel libdeps query failed:")
                        print(results)
                        sys.exit(1)
                    results = set(
                        [line for line in results.stdout.split("\n") if line.startswith("src/")]
                    )

                    for header in results:
                        fheaders.write(header + "\n")
                    for dep in deps:
                        fdeps.write(dep + "\n")

    return target, source


def generate(env):
    header_map = {}
    add_headers_from_all_libraries(env, header_map)
    gen_header_map = {}
    add_headers_from_gen_code(env, gen_header_map)
    target_libraries = {
        target_library.split("=")[-1].replace("\\", "/")
        for target_library in env.GetOption("bazel-includes-info")[0].split()
    }

    bazel_include_info = {
        "header_map": header_map,
        "gen_header_map": gen_header_map,
        "bazel_exec": env["SCONS2BAZEL_TARGETS"].bazel_executable,
        "config": env["BAZEL_FLAGS_STR"] + ["--config=local"],
    }

    with open(".bazel_include_info.json", "w") as f:
        json.dump(bazel_include_info, f)

    for builder_name in ["SharedLibrary", "StaticLibrary", "Program"]:
        builder = env["BUILDERS"][builder_name]
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter(
            [base_emitter, partial(bazel_includes_emitter, target_libraries)]
        )
        builder.emitter = new_emitter
