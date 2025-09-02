import errno
import fileinput
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from bazel.wrapper_hook.set_mongo_variables import write_mongo_variables_bazelrc


def run_pty_command(cmd):
    stdout = None
    try:
        import pty

        parent_fd, child_fd = pty.openpty()  # provide tty
        stdout = ""

        proc = subprocess.Popen(cmd, stdout=child_fd, stdin=child_fd)
        os.close(child_fd)
        while True:
            try:
                data = os.read(parent_fd, 512)
            except OSError as e:
                if e.errno != errno.EIO:
                    raise
                break  # EIO means EOF on some systems
            else:
                if not data:  # EOF
                    break
            stdout += data.decode()
    except ModuleNotFoundError:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
        )
        stdout = proc.stdout.decode()
    return stdout


def generate_compiledb(bazel_bin, persistent_compdb, enterprise):

    # compiledb ignores command line args so just make a version rc file in anycase
    write_mongo_variables_bazelrc([])
    if persistent_compdb:
        info_proc = subprocess.run(
            [bazel_bin, "info", "output_base"], capture_output=True, text=True
        )
        output_base = pathlib.Path(info_proc.stdout.strip() + "_bazel_compiledb")
        os.makedirs(REPO_ROOT / ".compiledb", exist_ok=True)
        symlink_prefix = REPO_ROOT / ".compiledb" / "compiledb-"

    compiledb_bazelrc = []
    compiledb_config = []
    if (REPO_ROOT / ".bazelrc.compiledb").exists():
        compiledb_bazelrc = ["--bazelrc=.bazelrc", "--bazelrc=.bazelrc.compiledb"]
    else:
        print(
            "Using default compiledb config, create a '.bazelrc.compiledb' file to customize the compiledb config..."
        )
        compiledb_config = ["--config=dbg"]

    if not enterprise:
        compiledb_config.append("--build_enterprise=False")

    query_cmd = (
        [bazel_bin]
        + ([f"--output_base={output_base}"] if persistent_compdb else [])
        + compiledb_bazelrc
        + ["aquery"]
        + ([f"--symlink_prefix={symlink_prefix}"] if persistent_compdb else [])
        + compiledb_config
        + [
            "--bes_backend=",
            "--bes_results_url=",
            "--noinclude_artifacts",
            'mnemonic("CppCompile|LinkCompile", //src/...)',
            "--output=jsonproto",
        ]
    )

    first_time = ""
    if persistent_compdb and not output_base.exists():
        first_time = " (the first time takes longer)"

    print(f"Generating compiledb command lines via aquery{first_time}...")
    stdout = run_pty_command(query_cmd)
    data = json.loads(stdout)

    output_json = []
    repo_root_resolved = str(REPO_ROOT.resolve())

    for action in data["actions"]:
        input_file = None
        output_file = None
        prev_arg = None
        for arg in reversed(action["arguments"]):
            if not input_file:
                if arg == "-c" or arg == "/c":
                    input_file = prev_arg
                elif arg.startswith("/c"):
                    input_file = arg[2:]
            if not output_file:
                if arg == "-o" or arg == "/Fo":
                    output_file = prev_arg
                elif arg.startswith("/Fo"):
                    output_file = arg[3:]
            if input_file and output_file:
                break
            prev_arg = arg

        if not input_file:
            raise Exception(
                f"failed to parse '-c' or '/c' from command line:{os.linesep}{' '.join(action['arguments'])}"
            )

        if not output_file:
            raise Exception(
                f"failed to parse '-o' or '/Fo' from command line:{os.linesep}{' '.join(action['arguments'])}"
            )

        if persistent_compdb:
            output_json.append(
                {
                    "file": input_file.replace("bazel-out", f"{symlink_prefix}out"),
                    "arguments": [
                        arg.replace("bazel-out", f"{symlink_prefix}out").replace(
                            "external/", f"{symlink_prefix}out/../../../external/"
                        )
                        for arg in action["arguments"]
                    ],
                    "directory": repo_root_resolved,
                    "output": output_file.replace("bazel-out", f"{symlink_prefix}out"),
                }
            )
        else:
            output_json.append(
                {
                    "file": input_file,
                    "arguments": action["arguments"],
                    "directory": repo_root_resolved,
                    "output": output_file,
                }
            )

    json_str = json.dumps(output_json, indent=4)
    compile_commands_json = REPO_ROOT / "compile_commands.json"
    need_rewrite = True
    if compile_commands_json.exists():
        with open(compile_commands_json, "r") as f:
            need_rewrite = json_str != f.read()

    if need_rewrite:
        with open(compile_commands_json, "w") as f:
            f.write(json_str)

    if not persistent_compdb:
        external_link = REPO_ROOT / "external"
        if external_link.exists():
            os.unlink(external_link)
        os.symlink(
            pathlib.Path(os.readlink(REPO_ROOT / "bazel-out")).parent.parent.parent / "external",
            external_link,
            target_is_directory=True,
        )

    print("Generating sources for compiledb...")
    gen_source_cmd = (
        [bazel_bin]
        + ([f"--output_base={output_base}"] if persistent_compdb else [])
        + compiledb_bazelrc
        + ["build"]
        + ([f"--symlink_prefix={symlink_prefix}"] if persistent_compdb else [])
        + compiledb_config
        + [
            f"--build_tag_filters=gen_source{',mongo-tidy-checks' if platform.system() != 'Windows' else ''}",
            "//src/...",
        ]
        + (["//:clang_tidy_config"] if platform.system() != "Windows" else [])
        + (["//:clang_tidy_config_strict"] if platform.system() != "Windows" else [])
    )
    run_pty_command(gen_source_cmd)

    if platform.system() != "Windows":
        clang_tidy_file = pathlib.Path(REPO_ROOT) / ".clang-tidy"

        if persistent_compdb:
            configs = [
                pathlib.Path(f"{symlink_prefix}bin") / config
                for config in [".clang-tidy.strict", ".clang-tidy"]
            ]
            for config in configs:
                os.chmod(config, 0o744)
                with fileinput.FileInput(config, inplace=True) as file:
                    for line in file:
                        print(line.replace("bazel-out/", f"{symlink_prefix}out/"), end="")
            shutil.copyfile(configs[1], clang_tidy_file)
            with open(".mongo_checks_module_path", "w") as f:
                f.write(
                    os.path.join(
                        f"{symlink_prefix}bin",
                        "src",
                        "mongo",
                        "tools",
                        "mongo_tidy_checks",
                        "libmongo_tidy_checks.so",
                    )
                )
        else:
            shutil.copyfile(pathlib.Path("bazel-bin") / ".clang-tidy", clang_tidy_file)


    if platform.system() == "Linux":
        # TODO: SERVER-110144 optimize this to only generate the extensions source code
        # instead of build the extension target entirely.
        gen_source_cmd = (
            [bazel_bin]
            + ([f"--output_base={output_base}"] if persistent_compdb else [])
            + compiledb_bazelrc
            + ["build"]
            + ([f"--symlink_prefix={symlink_prefix}"] if persistent_compdb else [])
            + compiledb_config
            + [
                "dist_test_extensions",
            ]
        )
        run_pty_command(gen_source_cmd)

    if persistent_compdb:
        shutdown_proc = subprocess.run(
            [bazel_bin, f"--output_base={output_base}", "shutdown"], capture_output=True, text=True
        )
        if shutdown_proc.returncode != 0:
            print(f"Failed to shutdown compiledb output_base: {shutdown_proc.returncode}")
            print("--- stdout ---:")
            print(shutdown_proc.stdout)
            print("--- stderr ---:")
            print(shutdown_proc.stderr)
    print("compiledb target done, finishing any other targets...")
