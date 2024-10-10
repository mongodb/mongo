# Generate linker commands in both Bazel and SCons for the intention of recording the differences
# during migration to Bazel. This will allow us to audit for differences if there are any issues
# found later on.

import argparse
import os
import platform
import re
import subprocess
import sys

from buildscripts.install_bazel import install_bazel

bazel_env_settings: dict[str, str] = {}


def scons_to_bazel_target(scons_target: str) -> str:
    # Example input: mongo/db/commands/libfsync_locked.a, output: //src/mongo/db/commands:fsync_locked
    # Remove lib prefix in filename
    scons_target = scons_target.replace("\\", "/")
    scons_target_parts = scons_target.split("/")
    if scons_target_parts[-1].startswith("lib"):
        scons_target = "/".join(scons_target_parts[:-1] + [scons_target_parts[-1][3:]])
    bazel_target = f"//src/{scons_target}"
    # Replace last / with :
    last_slash_idx = bazel_target.rfind("/")
    bazel_target = bazel_target[:last_slash_idx] + ":" + bazel_target[last_slash_idx + 1 :]
    # Remove suffix
    if "." in bazel_target:
        bazel_target = bazel_target[: bazel_target.rfind(".")]
    return bazel_target


def platformize_scons_target(scons_target: str) -> str:
    # Patch the target name to match the platform's file naming conventions.
    if platform.system() == "Windows":
        scons_target = scons_target.replace(".so", ".dll")
        scons_target = scons_target.replace(".a", ".lib")
        if not scons_target.endswith(".lib") and not scons_target.endswith(".dll"):
            scons_target += ".exe"
        scons_target_parts = scons_target.split("/")
        if scons_target_parts[-1].startswith("lib"):
            scons_target = "/".join(scons_target_parts[:-1] + [scons_target_parts[-1][3:]])
    elif platform.system() == "Darwin":
        scons_target = scons_target.replace(".so", ".dylib")
    return scons_target


def normalize_link_mode_ext(scons_target: str, link_static: bool) -> str:
    if link_static:
        scons_target = scons_target.replace(".so", ".a")
        scons_target = scons_target.replace(".dylib", ".a")
        scons_target = scons_target.replace(".dll", ".lib")
    else:
        if platform.system() == "Darwin":
            scons_target = scons_target.replace(".a", ".dylib")
        else:
            scons_target = scons_target.replace(".a", ".so")
        scons_target = scons_target.replace(".dll", ".lib")
    return scons_target


def log_subprocess_run(*args, **kwargs) -> subprocess.CompletedProcess:
    arg_list_or_string = kwargs["args"] if "args" in kwargs else args[0]
    print(" ".join(arg_list_or_string) if type(arg_list_or_string) == list else arg_list_or_string)
    try:
        proc = subprocess.run(*args, **kwargs)
        print(proc.stdout)
        print(proc.stderr)
        return proc
    except subprocess.CalledProcessError as e:
        print(e.stdout)
        print(e.stderr)
        raise e


def get_bazel_args(compiler_type: str, extra_args: list[str]) -> list[str]:
    # Do an actual run since dry-runs cannot create configure test entries which are needed for
    # getting the command line flags from dry runs later in the execution of this script.
    log_subprocess_run(
        [
            sys.executable,
            "buildscripts/scons.py",
            *extra_args,
            *(
                []
                if compiler_type is None
                else [f"--variables-files=etc/scons/mongodbtoolchain_stable_{compiler_type}.vars"]
            ),
            "VERBOSE=1",
            "ICECC=",
            "CCACHE=",
            "--ninja=disabled",
            "$BUILD_ROOT/scons/$VARIANT_DIR/sconf_temp",
        ],
        env={**os.environ.copy(), **bazel_env_settings},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )
    with open("build/scons/bazel/bazel_command", "r") as bazel_command_file:
        # strip off "bazel build"
        return bazel_command_file.read().split(" ")[2:]


def bazel_compile_commands(
    bazel_bin: str, bazel_targets: list[str], bazel_args: list[str]
) -> list[str]:
    debug_targets = " ".join([f'"{target}_with_debug"' for target in bazel_targets])
    proc = log_subprocess_run(
        [
            bazel_bin,
            "aquery",
            f'mnemonic("CppCompile", set({debug_targets}))',
            "--include_artifacts=false",
            *bazel_args,
        ],
        env={**os.environ.copy(), **bazel_env_settings},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )

    compile_lines_parts = []

    in_command = False
    for line in proc.stdout.split("\n"):
        if "Command Line:" in line:
            in_command = True
            compile_lines_parts.append([])
            line = line.replace("Command Line:", "")
            line = line.replace(" (exec ", "")

        if in_command:
            if not line.endswith("\\"):
                in_command = False

            # Remove the trailing \ or )
            if line.endswith("\\") or line.endswith(")"):
                line = line[:-1]
            compile_lines_parts[-1] += [line.strip()]

    bazel_output_files = set()
    compile_lines = []
    for i, compile_line_parts in enumerate(compile_lines_parts):
        # Move the -o {file} part to the beginning
        compiler_path = compile_line_parts[:1]
        if platform.system() == "Windows":
            # Windows appends /c
            output_args = [compile_line_parts[-3]]
            other_args = compile_line_parts[1:-3] + compile_line_parts[-2:]
        else:
            output_args = compile_line_parts[-2:]
            other_args = compile_line_parts[1:-2]
        if output_args[0].startswith("/Fo"):
            output_args[0] = output_args[0].replace("/Fo", "")
        compile_line_parts = compiler_path + output_args + other_args

        bazel_output_files.add(compile_line_parts[-1])

        compile_lines += [" ".join(compile_line_parts)]

        # Replace Bazel parts to match SCons output
        compile_lines[i] = re.sub(r"bazel-out/\S*/bin/src/", "", compile_lines[i])
        compile_lines[i] = re.sub(r"_objs/(\S*)_with_debug", r"\1", compile_lines[i])

    with open("./build/bazel-compile-commands", "w") as output_file:
        # Sort by output path
        compile_lines = sorted(compile_lines, key=lambda compile_line: compile_line.split(" ")[1])
        for compile_line in compile_lines:
            print(compile_line, file=output_file)

    return bazel_output_files


def bazel_linker_commands(
    bazel_bin: str,
    bazel_targets: list[str],
    target_type_map: dict[str, str],
    bazel_args: list[str],
    link_static: bool,
) -> list[str]:
    debug_targets = " ".join(
        [
            f'"{target}_shared_with_debug"'
            if target_type_map[target] == "library" and not link_static
            else f'"{target}_with_debug"'
            for target in bazel_targets
        ]
    )
    proc = log_subprocess_run(
        [
            bazel_bin,
            "aquery",
            f'mnemonic("CppLink", set({debug_targets}))',
            "--include_artifacts=false",
            *bazel_args,
        ],
        env={**os.environ.copy(), **bazel_env_settings},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )

    link_lines_parts = []

    in_command = False
    for line in proc.stdout.split("\n"):
        if "Command Line: (exec " in line:
            in_command = True
            link_lines_parts.append([])
            line = line.replace("Command Line: (exec ", "")

        if in_command:
            if not line.endswith("\\"):
                in_command = False

            # Remove the trailing \ or )
            line = line[:-1]
            link_lines_parts[-1] += [line.strip()]

    linker_output_paths = set()
    link_lines = []
    for i, link_line_parts in enumerate(link_lines_parts):
        # Remove the compiler path to match SCons output
        link_line_parts = link_line_parts[1:]

        # Move the -shared flag to the end to match SCons output
        if link_line_parts[0] == "-shared":
            link_line_parts = link_line_parts[1:] + link_line_parts[:1]

        link_lines += [" ".join(link_line_parts)]

        # Cleanup spammy linker flags
        link_lines[i] = re.sub(r"-Xlinker -rpath -Xlinker \S* ", "", link_lines[i])

        # Replace bazel paths to match SCons paths
        link_lines[i] = re.sub(r"bazel-out/\S*/bin/src/", "", link_lines[i])
        link_lines[i] = re.sub(r"_shared_with_debug", "", link_lines[i])
        link_lines[i] = re.sub(r"_with_debug", "", link_lines[i])

    linker_output_paths.update([link_line.split(" ")[1] for link_line in link_lines])

    with open("./build/bazel-link-commands", "w") as output_file:
        # Sort by output path
        link_lines = sorted(link_lines, key=lambda link_line: link_line.split(" ")[1])
        for link_line in link_lines:
            print(link_line, file=output_file)
    return linker_output_paths


def scons_commands(
    scons_targets: list[str],
    compiler_type: str,
    compile_output_paths: set[str],
    linker_output_paths: set[str],
    extra_args: list[str],
):
    log_subprocess_run(
        [
            sys.executable,
            "buildscripts/scons.py",
            *extra_args,
            *(
                []
                if compiler_type is None
                else [f"--variables-files=etc/scons/mongodbtoolchain_stable_{compiler_type}.vars"]
            ),
            "VERBOSE=1",
            "ICECC=",
            "CCACHE=",
            "--ninja=disabled",
            "$BUILD_ROOT/scons/$VARIANT_DIR/sconf_temp",
        ],
        env={**os.environ.copy(), **bazel_env_settings},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )
    proc = log_subprocess_run(
        [
            sys.executable,
            "buildscripts/scons.py",
            *extra_args,
            *(
                []
                if compiler_type is None
                else [f"--variables-files=etc/scons/mongodbtoolchain_stable_{compiler_type}.vars"]
            ),
            "VERBOSE=1",
            "ICECC=",
            "CCACHE=",
            "--ninja=disabled",
            "--dry-run",
            *scons_targets,
        ],
        env={**os.environ.copy(), **bazel_env_settings},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )

    with open("./build/scons-compile-commands", "w") as output_file:
        # Use -fno-omit-frame-pointer and /D_CRT_SECURE_NO_WARNINGS to identify compile commands since they're present on all compilations and will
        # not be removed in the lifetime of the hybrid build system.
        compile_commands = [
            output_line
            for output_line in proc.stdout.split("\n")
            if ("-fno-omit-frame-pointer" in output_line and "-o" in output_line)
            or "/D_CRT_SECURE_NO_WARNINGS" in output_line
        ]

        # Replace prefix to match Bazel
        compile_commands = [
            re.sub(r"build[\\/](opt|debug|san|optdebug)[\\/]", "", compile_command)
            for compile_command in compile_commands
        ]

        # Sort by output path
        compile_commands = sorted(
            compile_commands, key=lambda compile_command: compile_command.split(" ")[2]
        )

        compile_commands = [
            " ".join(compile_command.split(" ")) for compile_command in compile_commands
        ]

        for compile_command in compile_commands:
            # Skip over any lines with inputs not used in the Bazel compilation
            src_path = (
                compile_command.split(" ")[3].replace("\\", "/")
                if platform.system() == "Windows"
                else compile_command.split(" ")[-1]
            )
            if src_path not in compile_output_paths:
                continue
            print(compile_command, file=output_file)

    with open("./build/scons-link-commands", "w") as output_file:
        link_commands = [
            output_line
            for output_line in proc.stdout.split("\n")
            if ("-Wl," in output_line and "-o" in output_line)
            or "/LARGEADDRESSAWARE" in output_line
        ]

        # Remove binary name since it's not present in Bazel
        link_commands = [" ".join(link_command.split(" ")[1:]) for link_command in link_commands]

        # Replace prefix to match Bazel
        link_commands = [
            re.sub(r"build[\\/](opt|debug|san|optdebug)[\\/]", "", link_command)
            for link_command in link_commands
        ]

        # Sort by output path
        link_commands = sorted(link_commands, key=lambda link_command: link_command.split(" ")[1])

        link_commands = [" ".join(link_command.split(" ")) for link_command in link_commands]

        for link_command in link_commands:
            # Skip over any lines with outputs not referenced in the Bazel compilation
            if link_command.split(" ")[1] not in linker_output_paths:
                continue
            print(link_command, file=output_file)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--compiler_type",
        type=str,
        help="compiler type override, use for running locally",
        default=None,
    )
    parser.add_argument("--extra_args", type=str, help="list of args to pass to scons", default="")
    parser.add_argument(
        "scons_targets",
        nargs="+",
        help="List of SCons targets to compare with their Bazel equivalents. Remove the build/*config/ prefix. "
        + "Example: for build/fast/mongo/platform/visibility_test1 pass in mongo/platform/visibility_test1",
    )

    args = parser.parse_args()

    # Needed for git stash on Windows
    git_env = {
        **os.environ.copy(),
        **{
            "GIT_COMMITTER_NAME": "Evergreen",
            "GIT_COMMITTER_EMAIL": "evergreen@mongodb.com",
            "GIT_AUTHOR_NAME": "Evergreen",
            "GIT_AUTHOR_EMAIL": "evergreen@mongodb.com",
        },
    }

    # Switch to repository root directory.
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    # Set JAVA_HOME on ppc & s390x architectures
    global bazel_env_settings
    if platform.machine().lower() == "ppc64le":
        bazel_env_settings["JAVA_HOME"] = "/usr/lib/jvm/java-11-openjdk-11.0.4.11-2.el8.ppc64le"
    elif platform.machine().lower() == "s390x":
        bazel_env_settings["JAVA_HOME"] = "/usr/lib/jvm/java-11-openjdk-11.0.11.0.9-0.el8_3.s390x"

    target_type_map = {}
    for scons_target in args.scons_targets:
        basename = os.path.basename(scons_target)
        if basename.endswith((".a", ".lib", ".dylib", ".dll", ".so")):
            target_type = "library"
        elif (basename.endswith(".exe") and platform.system() == "Windows") or (
            "." not in (basename and platform.system() != "Windows")
        ):
            target_type = "binary"
        else:
            print("malformed target:", scons_target)
            exit(1)
        target_type_map[scons_to_bazel_target(scons_target)] = target_type

    extra_args = args.extra_args.strip().split(" ") if args.extra_args != "" else []

    # Replace the single quotes in the build command that would usually be removed by Bash
    extra_args = [extra_arg.replace("'", "") for extra_arg in extra_args]
    bazel_args = get_bazel_args(args.compiler_type, extra_args)
    link_static = "--//bazel/config:linkstatic=True" in bazel_args

    scons_targets = [
        "$BUILD_DIR/"
        + normalize_link_mode_ext(platformize_scons_target(scons_target), link_static).replace(
            "\\", "/"
        )
        for scons_target in args.scons_targets
    ]
    bazel_targets = [scons_to_bazel_target(scons_target) for scons_target in args.scons_targets]

    print("Bazel targets:", bazel_targets)
    print("SCons targets:", scons_targets)

    bazel_bin_dir = (
        os.getenv("TMPDIR") if os.getenv("TMPDIR") else os.path.expanduser("~/.local/bin")
    )
    if not os.path.exists(bazel_bin_dir):
        os.makedirs(bazel_bin_dir)

    bazel_bin = install_bazel(bazel_bin_dir)
    print("Bazel bin:", bazel_bin_dir)

    compile_output_paths = bazel_compile_commands(bazel_bin, bazel_targets, bazel_args)
    linker_output_paths = bazel_linker_commands(
        bazel_bin, bazel_targets, target_type_map, bazel_args, link_static
    )

    # With thin targets, the build system only allows one definition of each target between both SCons and Bazel.
    # Since we want to get a diff after removing the target from SCons and adding it to Bazel, we can rely on git
    # to revert the change to add the BUILD.bazel definitions when we want to execute the SCons version of the build.
    current_branch = subprocess.check_output(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"], env=git_env, text=True
    ).strip()
    subprocess.run(["git", "commit", "-m", "tmp"], env=git_env)
    subprocess.run(["git", "checkout", "HEAD~1"], env=git_env, check=True)
    scons_commands(
        scons_targets, args.compiler_type, compile_output_paths, linker_output_paths, extra_args
    )

    with open("./build/merged_diff.md", "w") as output_file:
        output_file.write(" ".join(sys.argv) + "\n\n")
        for file_name in [
            "scons-compile-commands",
            "bazel-compile-commands",
            "scons-link-commands",
            "bazel-link-commands",
        ]:
            with open(f"./build/{file_name}", "r") as input_file:
                output_file.write(f"{file_name}:\n\n")
                print(f"{file_name}:\n\n")
                output_file.write("```\n")
                for line in input_file:
                    output_file.write(line)
                    print(line)
                print("\n\n\n")
                output_file.write("\n```\n\n\n")
    subprocess.run(["git", "checkout", current_branch], env=git_env, check=True)
    subprocess.run(["git", "reset", "HEAD~1"], env=git_env, check=True)


if __name__ == "__main__":
    main()
