import atexit
import errno
import getpass
import glob
import hashlib
import json
import os
import platform
import queue
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import threading
import time
import traceback
import urllib.request
from io import StringIO
from typing import Any, Dict, List, Set, Tuple

import distro
import git
import psutil
import requests
import SCons
from retry import retry
from retry.api import retry_call
from SCons.Script import ARGUMENTS

from buildscripts.install_bazel import install_bazel
from buildscripts.util.read_config import read_config_file
from evergreen.api import RetryingEvergreenApi

# Disable retries locally
_LOCAL_MAX_RETRY_ATTEMPTS = 1

# Enable up to 3 attempts in
_CI_MAX_RETRY_ATTEMPTS = 3

_SUPPORTED_PLATFORM_MATRIX = [
    "linux:arm64:gcc",
    "linux:arm64:clang",
    "linux:amd64:gcc",
    "linux:amd64:clang",
    "linux:ppc64le:gcc",
    "linux:ppc64le:clang",
    "linux:s390x:gcc",
    "linux:s390x:clang",
    "windows:amd64:msvc",
    "macos:amd64:clang",
    "macos:arm64:clang",
]

_SANITIZER_MAP = {
    "address": "asan",
    "fuzzer": "fsan",
    "memory": "msan",
    "leak": "lsan",
    "thread": "tsan",
    "undefined": "ubsan",
}

_DISTRO_PATTERN_MAP = {
    "Ubuntu 18*": "ubuntu18",
    "Ubuntu 20*": "ubuntu20",
    "Ubuntu 22*": "ubuntu22",
    "Ubuntu 24*": "ubuntu24",
    "Amazon Linux 2": "amazon_linux_2",
    "Amazon Linux 2023": "amazon_linux_2023",
    "Debian GNU/Linux 10": "debian10",
    "Debian GNU/Linux 12": "debian12",
    "Red Hat Enterprise Linux 8*": "rhel8",
    "Red Hat Enterprise Linux 9*": "rhel9",
    "SLES 15*": "suse15",
}

_S3_HASH_MAPPING = {
    "https://mdb-build-public.s3.amazonaws.com/bazel-binaries/bazel-7.2.1-ppc64le": "4ecc7f1396b8d921c6468b34cc8ed356c4f2dbe8a154c25d681a61ccb5dfc9cb",
    "https://mdb-build-public.s3.amazonaws.com/bazel-binaries/bazel-7.2.1-s390x": "2f5f7fd747620d96e885766a4027347c75c0f455c68219211a00e72fc6413be9",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-darwin-amd64": "f2ba5f721a995b54bab68c6b76a340719888aa740310e634771086b6d1528ecd",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-darwin-arm64": "69fa21cd2ccffc2f0970c21aa3615484ba89e3553ecce1233a9d8ad9570d170e",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-linux-amd64": "d28b588ac0916abd6bf02defb5433f6eddf7cba35ffa808eabb65a44aab226f7",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-linux-arm64": "861a16ba9979613e70bd3d2f9d9ab5e3b59fe79471c5753acdc9c431ab6c9d94",
    "https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-windows-amd64.exe": "d04555245a99dfb628e33da24e2b9198beb8f46d7e7661c313eb045f6a59f5e4",
}


class Globals:
    # key: scons target, value: {bazel target, bazel output}
    scons2bazel_targets: Dict[str, Dict[str, str]] = dict()

    # key: scons output, value: bazel outputs
    scons_output_to_bazel_outputs: Dict[str, List[str]] = dict()

    # targets bazel needs to build
    bazel_targets_work_queue: queue.Queue[str] = queue.Queue()

    # targets bazel has finished building
    bazel_targets_done: Set[str] = set()

    # lock for accessing the targets done list
    bazel_target_done_CV: threading.Condition = threading.Condition()

    # bazel command line with options, but not targets
    bazel_base_build_command: List[str] = None

    # environment variables to set when invoking bazel
    bazel_env_variables: Dict[str, str] = {}

    # Flag to signal that scons is ready to build, but needs to wait on bazel
    waiting_on_bazel_flag: bool = False

    # Flag to signal that scons is ready to build, but needs to wait on bazel
    bazel_build_success: bool = False

    bazel_build_exitcode: int = 1

    # a IO object to hold the bazel output in place of stdout
    bazel_thread_terminal_output = StringIO()

    bazel_executable = None

    max_retry_attempts: int = _LOCAL_MAX_RETRY_ATTEMPTS

    bazel_remote_timeout: int = -1

    timeout_event = threading.Event()

    # Timeout when stuck scheduling without making progress for more than 15 minutes
    # Ex string:
    # [21,537 / 21,603] [Sched] Compiling src/mongo/db/s/migration_chunk_cloner_source.cpp; 1424s
    last_sched_target_progress = ""
    sched_time_start = 0
    sched_timeout_sec = 60 * 15

    @staticmethod
    def bazel_output(scons_node):
        return Globals.scons2bazel_targets[str(scons_node).replace("\\", "/")]["bazel_output"]

    @staticmethod
    def bazel_target(scons_node):
        return Globals.scons2bazel_targets[str(scons_node).replace("\\", "/")]["bazel_target"]

    @staticmethod
    def bazel_link_file(scons_node):
        bazel_target = Globals.scons2bazel_targets[str(scons_node).replace("\\", "/")][
            "bazel_target"
        ]
        linkfile = bazel_target.replace("//src/", "bazel-bin/src/") + "_links.list"
        return "/".join(linkfile.rsplit(":", 1))

    @staticmethod
    def bazel_sources_file(scons_node):
        bazel_target = Globals.scons2bazel_targets[str(scons_node).replace("\\", "/")][
            "bazel_target"
        ]
        sources_file = (
            bazel_target.replace("//src/", "bazel-bin/src/") + "_sources_list.sources_list"
        )
        return "/".join(sources_file.rsplit(":", 1))


def bazel_debug(msg: str):
    pass


def bazel_target_emitter(
    target: List[SCons.Node.Node], source: List[SCons.Node.Node], env: SCons.Environment.Environment
) -> Tuple[List[SCons.Node.Node], List[SCons.Node.Node]]:
    """This emitter will map any scons outputs to bazel outputs so copy can be done later."""

    for t in target:
        # bazel will cache the results itself, don't recache
        env.NoCache(t)

    return (target, source)


def bazel_builder_action(
    env: SCons.Environment.Environment, target: List[SCons.Node.Node], source: List[SCons.Node.Node]
):
    if env.GetOption("separate-debug") == "on":
        shlib_suffix = env.subst("$SHLIBSUFFIX")
        sep_dbg = env.subst("$SEPDBG_SUFFIX")
        if sep_dbg and str(target[0]).endswith(shlib_suffix):
            target.append(env.File(str(target[0]) + sep_dbg))

    # now copy all the targets out to the scons tree, note that target is a
    # list of nodes so we need to stringify it for copyfile
    for t in target:
        dSYM_found = False
        if ".dSYM/" in str(t):
            # ignore dSYM plist file, as we skipped it prior
            if str(t).endswith(".plist"):
                continue

            dSYM_found = True

        if dSYM_found:
            # Here we handle the difference between scons and bazel for dSYM dirs. SCons uses list
            # actions to perform operations on the same target during some action. Bazel does not
            # have an exact corresponding feature. Each action in bazel should have unique inputs and
            # outputs. The file and targets wont line up exactly between scons and our mongo_cc_library,
            # custom rule, specifically the way dsymutil generates the dwarf file inside the dSYM dir. So
            # we remap the special filename suffixes we use for our bazel intermediate cc_library rules.
            #
            # So we will do the renaming of dwarf file to what scons expects here, before we copy to scons tree
            substring_end = str(t).find(".dSYM/") + 5
            t = str(t)[:substring_end]
            # This is declared as an output folder, so bazel appends (TreeArtifact) to it
            s = Globals.bazel_output(t + " (TreeArtifact)")
            s = str(s).removesuffix(" (TreeArtifact)")
            dwarf_info_base = os.path.splitext(os.path.splitext(os.path.basename(t))[0])[0]
            dwarf_sym_with_debug = os.path.join(
                s, f"Contents/Resources/DWARF/{dwarf_info_base}_shared_with_debug.dylib"
            )

            # this handles shared libs or program binaries
            if os.path.exists(dwarf_sym_with_debug):
                dwarf_sym = os.path.join(s, f"Contents/Resources/DWARF/{dwarf_info_base}.dylib")
            else:
                dwarf_sym_with_debug = os.path.join(
                    s, f"Contents/Resources/DWARF/{dwarf_info_base}_with_debug"
                )
                dwarf_sym = os.path.join(s, f"Contents/Resources/DWARF/{dwarf_info_base}")

            # copy the whole dSYM in one operation. Clean any existing files that might be in the way.
            print(f"Moving .dSYM from {s} over to {t}.")
            shutil.rmtree(str(t), ignore_errors=True)
            shutil.copytree(s, str(t))
            # we want to change the permissions back to normal permissions on the folders copied rather than read only
            os.chmod(t, 0o755)
            for root, dirs, files in os.walk(t):
                for name in files:
                    os.chmod(os.path.join(root, name), 0o755)
                for name in dirs:
                    os.chmod(os.path.join(root, name), 0o755)
            # shouldn't write our own files to the bazel directory, renaming file for scons
            shutil.copy(dwarf_sym_with_debug.replace(s, t), dwarf_sym.replace(s, t))
        else:
            s = Globals.bazel_output(t)
            try:
                # Check if the current directory and .cache files are on the same mount
                # because hardlinking doesn't work between drives and when it fails
                # it leaves behind a symlink that is hard to clean up
                # We don't hardlink on windows because SCons will run link commands against
                # the files in the bazel directory, and if its running the link command
                # while SCons cleans up files in the output directory you get file permission errors
                if (
                    platform.system() != "Windows"
                    and os.stat(".").st_dev == os.stat(s, follow_symlinks=True).st_dev
                ):
                    if os.path.exists(str(t)):
                        os.remove(str(t))
                    os.link(s, str(t))
                    os.chmod(str(t), os.stat(str(t)).st_mode | stat.S_IWUSR)
                else:
                    print(
                        f"Copying {s} to {t} instead of hardlinking because files are on different mounts or we are on Windows."
                    )
                    shutil.copy(s, str(t))
                    os.chmod(str(t), os.stat(str(t)).st_mode | stat.S_IWUSR)
            # Fall back on the original behavior of copying, likely if we hit here this
            # will still fail due to hardlinking leaving some symlinks around
            except Exception as e:
                print(e)
                print(f"Failed to hardlink {s} to {t}, trying to copying file instead.")
                shutil.copy(s, str(t))
                os.chmod(str(t), os.stat(str(t)).st_mode | stat.S_IWUSR)


BazelCopyOutputsAction = SCons.Action.FunctionAction(
    bazel_builder_action,
    {"cmdstr": "Hardlinking $TARGETS from bazel build directory.", "varlist": ["BAZEL_FLAGS_STR"]},
)

total_query_time = 0
total_queries = 0


def bazel_query_func(
    env: SCons.Environment.Environment, query_command_args: List[str], query_name: str = "query"
):
    full_command = [Globals.bazel_executable] + query_command_args
    global total_query_time, total_queries
    start_time = time.time()
    # these args prune the graph we need to search through a bit since we only care about our
    # specific library target dependencies
    full_command += ["--implicit_deps=False", "--tool_deps=False", "--include_aspects=False"]
    # prevent remote connection and invocations since we just want to query the graph
    full_command += [
        "--remote_executor=",
        "--remote_cache=",
        "--bes_backend=",
        "--bes_results_url=",
    ]
    bazel_debug(f"Running query: {' '.join(full_command)}")
    results = subprocess.run(
        full_command,
        capture_output=True,
        text=True,
        cwd=env.Dir("#").abspath,
        env={**os.environ.copy(), **Globals.bazel_env_variables},
    )
    delta = time.time() - start_time
    bazel_debug(f"Spent {delta} seconds running {query_name}")
    total_query_time += delta
    total_queries += 1

    # Manually throw the error instead of using subprocess.run(... check=True) to print out stdout and stderr.
    if results.returncode != 0:
        print(results.stdout)
        print(results.stderr)
        raise subprocess.CalledProcessError(
            results.returncode, full_command, results.stdout, results.stderr
        )
    return results


# the ninja tool has some API that doesn't support using SCons env methods
# instead of adding more API to the ninja tool which has a short life left
# we just add the unused arg _dup_env
def ninja_bazel_builder(
    env: SCons.Environment.Environment,
    _dup_env: SCons.Environment.Environment,
    node: SCons.Node.Node,
) -> Dict[str, Any]:
    """
    Translator for ninja which turns the scons bazel_builder_action
    into a build node that ninja can digest.
    """

    outs = env.NinjaGetOutputs(node)
    ins = [Globals.bazel_output(out) for out in outs]

    # this represents the values the ninja_syntax.py will use to generate to real
    # ninja syntax defined in the ninja manaul: https://ninja-build.org/manual.html#ref_ninja_file
    return {
        "outputs": outs,
        "inputs": ins,
        "rule": "BAZEL_COPY_RULE",
        "variables": {
            "cmd": " && ".join(
                [
                    f"$COPY {input_node.replace('/',os.sep)} {output_node}"
                    for input_node, output_node in zip(ins, outs)
                ]
                + [
                    # Touch output files to make sure that the modified time of inputs is always older than the modified time of outputs.
                    f"copy /b {output_node} +,, {output_node}"
                    if env["PLATFORM"] == "win32"
                    else f"touch {output_node}"
                    for output_node in outs
                ]
            )
        },
    }


def write_bazel_build_output(line: str) -> None:
    if Globals.waiting_on_bazel_flag:
        if Globals.bazel_thread_terminal_output is not None:
            Globals.bazel_thread_terminal_output.seek(0)
            sys.stdout.write(Globals.bazel_thread_terminal_output.read())
            Globals.bazel_thread_terminal_output = None
        sys.stdout.write(line)
    else:
        Globals.bazel_thread_terminal_output.write(line)


def bazel_server_timeout_dumper(jvm_out, proc_pid, project_root):
    p = psutil.Process(proc_pid)

    Globals.timeout_event.wait()
    if p.is_running():
        os.kill(int(proc_pid), signal.SIGTERM)
        p.wait()

        if os.path.exists(".bazel_real"):
            with tarfile.open(os.path.join(project_root, "jvm.out.tar.gz"), "w:gz") as tar:
                tar.add(jvm_out)

        try:
            expansions = read_config_file(os.path.join(project_root, "../expansions.yml"))
            task_id = expansions.get("task_id", None)
            error_msg = (
                "Bazel timed out waiting for remote action (from BF-35762).\n"
                f"See task: <https://spruce.mongodb.com/task/{task_id}|here>."
            )

            evg_api = RetryingEvergreenApi.get_api(
                config_file=os.path.join(project_root, ".evergreen.yml")
            )
            evg_api.send_slack_message(
                target="#devprod-build-triager",
                msg=error_msg,
            )
        except Exception:  # pylint: disable=broad-except
            traceback.print_exc()


def bazel_build_subproc_func(**kwargs):
    project_root = os.path.abspath(".")
    output_base = subprocess.run(
        [Globals.bazel_executable, "info", "output_base"],
        capture_output=True,
        text=True,
        check=True,
        env=kwargs["env"],
    ).stdout.strip()
    if os.path.exists(".bazel_real"):
        with open(".bazel_real") as f:
            kwargs["args"][0] = f.read().strip()
    jvm_out = os.path.join(output_base, "server/jvm.out")

    bazel_proc = subprocess.Popen(**kwargs)

    t = threading.Thread(
        target=bazel_server_timeout_dumper,
        args=(jvm_out, bazel_proc.pid, project_root),
    )

    # the bazel calls are wrapped in retries so we can rely on them to restart the attempt.
    t.daemon = True
    t.start()

    return bazel_proc


def check_timeout_condition(line):
    if "[Sched]" in line:
        target_progress = line.split("[Sched]")[0].strip()
        if len(target_progress) > 0:
            if Globals.last_sched_target_progress == target_progress:
                if time.time() - Globals.sched_time_start > Globals.sched_timeout_sec:
                    Globals.last_sched_target_progress = ""
                    write_bazel_build_output("Stuck scheduling for too long, terminating")
                    Globals.timeout_event.set()
                    return True
            else:
                Globals.sched_time_start = time.time()
            Globals.last_sched_target_progress = target_progress


def perform_tty_bazel_build(bazel_cmd: str) -> None:
    # Importing pty will throw on certain platforms, the calling code must catch this exception
    # and fallback to perform_non_tty_bazel_build.
    import pty

    parent_fd, child_fd = pty.openpty()  # provide tty
    bazel_proc = bazel_build_subproc_func(
        args=bazel_cmd,
        stdin=child_fd,
        stdout=child_fd,
        stderr=subprocess.STDOUT,
        env={**os.environ.copy(), **Globals.bazel_env_variables},
    )

    buffer = ""
    os.close(child_fd)
    Globals.timeout_event.clear()
    Globals.last_sched_target_progress = ""
    Globals.sched_time_start = time.time()
    try:
        # This loop will terminate with an EOF or EOI when the process ends.
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

            write_bazel_build_output(data.decode())
            buffer += data.decode()
            if "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                if check_timeout_condition(line):
                    raise subprocess.CalledProcessError(-1, bazel_cmd, "", "")
    finally:
        os.close(parent_fd)
        if bazel_proc.poll() is None:
            bazel_proc.terminate()
        bazel_proc.wait()

    Globals.bazel_build_exitcode = bazel_proc.returncode

    if bazel_proc.returncode != 0:
        raise subprocess.CalledProcessError(bazel_proc.returncode, bazel_cmd, "", "")


def perform_non_tty_bazel_build(bazel_cmd: str) -> None:
    bazel_proc = bazel_build_subproc_func(
        args=bazel_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env={**os.environ.copy(), **Globals.bazel_env_variables},
        text=True,
    )
    Globals.timeout_event.clear()
    Globals.last_sched_target_progress = ""
    Globals.sched_time_start = time.time()

    # This loop will terminate when the process ends.
    while True:
        line = bazel_proc.stdout.readline()
        if not line:
            break
        write_bazel_build_output(line)
        if check_timeout_condition(line):
            raise subprocess.CalledProcessError(-1, bazel_cmd, "", "")

    stdout, stderr = bazel_proc.communicate()

    Globals.bazel_build_exitcode = bazel_proc.returncode

    if bazel_proc.returncode != 0:
        raise subprocess.CalledProcessError(bazel_proc.returncode, bazel_cmd, stdout, stderr)


def run_bazel_command(env, bazel_cmd, tries_so_far=0):
    try:
        tty_import_fail = False
        try:
            retry_call(
                perform_tty_bazel_build,
                [bazel_cmd],
                tries=Globals.max_retry_attempts,
                exceptions=(subprocess.CalledProcessError,),
            )
        except ImportError:
            # Run the actual build outside of the except clause to avoid confusion in the stack trace,
            # otherwise, build failures on platforms that don't support tty will be displayed as import errors.
            tty_import_fail = True
            pass

        if tty_import_fail:
            retry_call(
                perform_non_tty_bazel_build,
                [bazel_cmd],
                tries=Globals.max_retry_attempts,
                exceptions=(subprocess.CalledProcessError,),
            )
    except subprocess.CalledProcessError as ex:
        if platform.system() == "Windows" and tries_so_far == 0:
            print(
                "Build failed, retrying with --jobs=4 in case linking failed due to hitting concurrency limits..."
            )
            run_bazel_command(
                env, bazel_cmd + ["--jobs", "4", "--link_timeout_8min=False"], tries_so_far=1
            )
            return

        print("ERROR: Bazel build failed:")
        Globals.timeout_event.set()

        if Globals.bazel_thread_terminal_output is not None:
            Globals.bazel_thread_terminal_output.seek(0)
            ex.output += Globals.bazel_thread_terminal_output.read()
            Globals.bazel_thread_terminal_output = None
            print(ex.output)

        raise ex
    Globals.bazel_build_success = True
    Globals.timeout_event.set()


def bazel_build_thread_func(env, log_dir: str, verbose: bool, ninja_generate: bool) -> None:
    """This thread runs the bazel build up front."""

    if verbose:
        extra_args = []
    else:
        extra_args = ["--output_filter=DONT_MATCH_ANYTHING"]

    if ninja_generate:
        for file in glob.glob("bazel-out/**/*.gen_source_list", recursive=True):
            os.remove(file)
        extra_args += ["--build_tag_filters=scons_link_lists"]
        bazel_cmd = Globals.bazel_base_build_command + extra_args + ["//src/..."]

    elif SCons.Script.BUILD_TARGETS == ["compiledb"]:
        extra_args += ["--build_tag_filters=scons_link_lists,compiledb,gen_source"]
        bazel_cmd = Globals.bazel_base_build_command + extra_args + ["//:compiledb", "//src/..."]
    elif SCons.Script.BUILD_TARGETS == ["compiledb", "+mongo-tidy-tests"]:
        extra_args += [
            "--build_tag_filters=scons_link_lists,compiledb,gen_source,mongo-tidy-tests,mongo-tidy-checks"
        ]
        bazel_cmd = Globals.bazel_base_build_command + extra_args + ["//:compiledb", "//src/..."]
    else:
        build_tags = env.GetOption("bazel-build-tag")
        if not build_tags:
            build_tags += ["all"]
        if "all" not in build_tags:
            build_tags += ["scons_link_lists", "gen_source"]
            extra_args += [f"--build_tag_filters={','.join(build_tags)}"]
        bazel_cmd = Globals.bazel_base_build_command + extra_args + ["//src/..."]

    if ninja_generate:
        print("Generating bazel link deps...")
    else:
        print(f"Bazel build command:\n{' '.join(bazel_cmd)}")

    if env.GetOption("coverity-build"):
        print(f"BAZEL_COMMAND: {' '.join(bazel_cmd)}")
        return

    print("Starting bazel build thread...")
    run_bazel_command(env, bazel_cmd)


def create_bazel_builder(builder: SCons.Builder.Builder) -> SCons.Builder.Builder:
    return SCons.Builder.Builder(
        action=BazelCopyOutputsAction,
        prefix=builder.prefix,
        suffix=builder.suffix,
        src_suffix=builder.src_suffix,
        source_scanner=builder.source_scanner,
        target_scanner=builder.target_scanner,
        emitter=SCons.Builder.ListEmitter([builder.emitter, bazel_target_emitter]),
    )


# TODO delete this builder when we have testlist support in bazel
def create_program_builder(env: SCons.Environment.Environment) -> None:
    env["BUILDERS"]["BazelProgram"] = create_bazel_builder(env["BUILDERS"]["Program"])


def get_default_cert_dir():
    if platform.system() == "Windows":
        return f"C:/cygwin/home/{getpass.getuser()}/.engflow"
    elif platform.system() == "Linux":
        return f"/home/{getpass.getuser()}/.engflow"
    elif platform.system() == "Darwin":
        return f"{os.path.expanduser('~')}/.engflow"


def validate_remote_execution_certs(env: SCons.Environment.Environment) -> bool:
    running_in_evergreen = os.environ.get("CI")

    if running_in_evergreen and not os.path.exists("./engflow.cert"):
        print(
            "ERROR: ./engflow.cert not found, which is required to build in evergreen without BAZEL_FLAGS=--config=local set. Please reach out to #ask-devprod-build for help."
        )
        return False

    if os.name == "nt" and not os.path.exists(f"{os.path.expanduser('~')}/.bazelrc"):
        with open(f"{os.path.expanduser('~')}/.bazelrc", "a") as bazelrc:
            bazelrc.write(
                f"build --tls_client_certificate={get_default_cert_dir()}/creds/engflow.crt\n"
            )
            bazelrc.write(f"build --tls_client_key={get_default_cert_dir()}/creds/engflow.key\n")

    if not running_in_evergreen and not os.path.exists(
        f"{get_default_cert_dir()}/creds/engflow.crt"
    ):
        # Temporary logic to copy over the credentials for users that ran the installation steps using the old directory (/engflow/).
        if os.path.exists("/engflow/creds/engflow.crt") and os.path.exists(
            "/engflow/creds/engflow.key"
        ):
            print(
                "Moving EngFlow credentials from the legacy directory (/engflow/) to the new directory (~/.engflow/)."
            )
            try:
                os.makedirs(f"{get_default_cert_dir()}/creds/", exist_ok=True)
                shutil.move(
                    "/engflow/creds/engflow.crt",
                    f"{get_default_cert_dir()}/creds/engflow.crt",
                )
                shutil.move(
                    "/engflow/creds/engflow.key",
                    f"{get_default_cert_dir()}/creds/engflow.key",
                )
                with open(f"{get_default_cert_dir()}/.bazelrc", "a") as bazelrc:
                    bazelrc.write(
                        f"build --tls_client_certificate={get_default_cert_dir()}/creds/engflow.crt\n"
                    )
                    bazelrc.write(
                        f"build --tls_client_key={get_default_cert_dir()}/creds/engflow.key\n"
                    )
            except OSError as exc:
                print(exc)
                print(
                    "Failed to update cert location, please move them manually. Otherwise you can pass 'BAZEL_FLAGS=\"--config=local\"' on the SCons command line."
                )

            return True

        # Pull the external hostname of the system from aws
        try:
            response = requests.get(
                "http://instance-data.ec2.internal/latest/meta-data/public-hostname"
            )
            status_code = response.status_code
        except Exception as _:
            status_code = 500
        if status_code == 200:
            public_hostname = response.text
        else:
            public_hostname = "localhost"
        print(
            f"""\nERROR: {get_default_cert_dir()}/creds/engflow.crt not found. Please reach out to #ask-devprod-build if you need help with the steps below.

(If the below steps are not working or you are an external person to MongoDB, remote execution can be disabled by passing BAZEL_FLAGS=--config=local at the end of your scons.py invocation)

Please complete the following steps to generate a certificate:
- (If not in the Engineering org) Request access to the MANA group https://mana.corp.mongodbgov.com/resources/659ec4b9bccf3819e5608712
- Go to https://sodalite.cluster.engflow.com/gettingstarted (Uses mongodbcorp.okta.com auth URL)
- Login with OKTA, then click the \"GENERATE AND DOWNLOAD MTLS CERTIFICATE\" button
  - (If logging in with OKTA doesn't work) Login with Google using your MongoDB email, then click the "GENERATE AND DOWNLOAD MTLS CERTIFICATE" button
- On your local system (usually your MacBook), open a terminal and run:

ZIP_FILE=~/Downloads/engflow-mTLS.zip

curl https://raw.githubusercontent.com/mongodb/mongo/master/buildscripts/setup_engflow_creds.sh -o setup_engflow_creds.sh
chmod +x ./setup_engflow_creds.sh
./setup_engflow_creds.sh {getpass.getuser()} {public_hostname} $ZIP_FILE {"local" if public_hostname == "localhost" else ""}\n"""
        )
        return False

    if not running_in_evergreen and (
        not os.access(f"{get_default_cert_dir()}/creds/engflow.crt", os.R_OK)
        or not os.access(f"{get_default_cert_dir()}/creds/engflow.key", os.R_OK)
    ):
        print(
            f"Invalid permissions set on {get_default_cert_dir()}/creds/engflow.crt or {get_default_cert_dir()}/creds/engflow.key"
        )
        print("Please run the following command to fix the permissions:\n")
        print(
            f"sudo chown {getpass.getuser()}:{getpass.getuser()} {get_default_cert_dir()}/creds/engflow.crt {get_default_cert_dir()}/creds/engflow.key"
        )
        print(
            f"sudo chmod 600 {get_default_cert_dir()}/creds/engflow.crt {get_default_cert_dir()}/creds/engflow.key"
        )
        return False
    return True


def generate_bazel_info_for_ninja(env: SCons.Environment.Environment) -> None:
    # create a json file which contains all the relevant info from this generation
    # that bazel will need to construct the correct command line for any given targets
    ninja_bazel_build_json = {
        "bazel_cmd": Globals.bazel_base_build_command,
        "compiledb_cmd": [Globals.bazel_executable, "run"]
        + env["BAZEL_FLAGS_STR"]
        + ["//:compiledb", "--"]
        + env["BAZEL_FLAGS_STR"],
        "defaults": [str(t) for t in SCons.Script.DEFAULT_TARGETS],
        "targets": Globals.scons2bazel_targets,
        "CC": env.get("CC", ""),
        "CXX": env.get("CXX", ""),
        "USE_NATIVE_TOOLCHAIN": os.environ.get("USE_NATIVE_TOOLCHAIN"),
    }
    with open(f".{env.subst('$NINJA_PREFIX')}.bazel_info_for_ninja.txt", "w") as f:
        json.dump(ninja_bazel_build_json, f)

    # we also store the outputs in the env (the passed env is intended to be
    # the same main env ninja tool is constructed with) so that ninja can
    # use these to contruct a build node for running bazel where bazel list the
    # correct bazel outputs to be copied to the scons tree. We also handle
    # calculating the inputs. This will be the all the inputs of the outs,
    # but and input can not also be an output. If a node is found in both
    # inputs and outputs, remove it from the inputs, as it will be taken care
    # internally by bazel build.
    ninja_bazel_outs = []
    ninja_bazel_ins = []
    for scons_t, bazel_t in Globals.scons2bazel_targets.items():
        ninja_bazel_outs += [bazel_t["bazel_output"]]
        ninja_bazel_ins += env.NinjaGetInputs(env.File(scons_t))

    if platform.system() == "Linux" and not os.environ.get("USE_NATIVE_TOOLCHAIN"):
        ninja_bazel_outs += [env.get("CC"), env.get("CXX")]

    # This is to be used directly by ninja later during generation of the ninja file
    env["NINJA_BAZEL_OUTPUTS"] = ninja_bazel_outs
    env["NINJA_BAZEL_INPUTS"] = ninja_bazel_ins


@retry(tries=5, delay=3)
def download_path_with_retry(*args, **kwargs):
    urllib.request.urlretrieve(*args, **kwargs)


install_query_cache = {}


def bazel_deps_check_query_cache(env, bazel_target):
    return install_query_cache.get(bazel_target, None)


def bazel_deps_add_query_cache(env, bazel_target, results):
    install_query_cache[bazel_target] = results


link_query_cache = {}


def bazel_deps_check_link_query_cache(env, bazel_target):
    return link_query_cache.get(bazel_target, None)


def bazel_deps_add_link_query_cache(env, bazel_target, results):
    link_query_cache[bazel_target] = results


def sha256_file(filename: str) -> str:
    sha256_hash = hashlib.sha256()
    with open(filename, "rb") as f:
        for block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(block)
        return sha256_hash.hexdigest()


def verify_s3_hash(s3_path: str, local_path: str) -> None:
    if s3_path not in _S3_HASH_MAPPING:
        raise Exception(
            "S3 path not found in hash mapping, unable to verify downloaded for s3 path: s3_path"
        )

    hash = sha256_file(local_path)
    if hash != _S3_HASH_MAPPING[s3_path]:
        raise Exception(
            f"Hash mismatch for {s3_path}, expected {_S3_HASH_MAPPING[s3_path]} but got {hash}"
        )


def find_distro_match(distro_str: str) -> str:
    for distro_pattern, simplified_name in _DISTRO_PATTERN_MAP.items():
        if "*" in distro_pattern:
            prefix_suffix = distro_pattern.split("*")
            if distro_str.startswith(prefix_suffix[0]) and distro_str.endswith(prefix_suffix[1]):
                return simplified_name
        elif distro_str == distro_pattern:
            return simplified_name
    return None


time_auto_installing = 0
count_of_auto_installing = 0


def timed_auto_install_bazel(env, libdep, shlib_suffix):
    global time_auto_installing, count_of_auto_installing
    start_time = time.time()
    auto_install_bazel(env, libdep, shlib_suffix)
    time_auto_installing += time.time() - start_time
    count_of_auto_installing += 1


def auto_install_single_target(env, libdep, suffix, bazel_node):
    auto_install_mapping = env["AIB_SUFFIX_MAP"].get(suffix)

    env.AutoInstall(
        target=auto_install_mapping.directory,
        source=[bazel_node],
        AIB_COMPONENT=env.get("AIB_COMPONENT", "AIB_DEFAULT_COMPONENT"),
        AIB_ROLE=auto_install_mapping.default_role,
        AIB_COMPONENTS_EXTRA=env.get("AIB_COMPONENTS_EXTRA", []),
    )
    auto_installed_libdep = env.GetAutoInstalledFiles(libdep)
    auto_installed_bazel_node = env.GetAutoInstalledFiles(bazel_node)

    if auto_installed_libdep[0] != auto_installed_bazel_node[0]:
        env.Depends(auto_installed_libdep[0], auto_installed_bazel_node[0])

    return env.GetAutoInstalledFiles(bazel_node)


def auto_install_bazel(env, libdep, shlib_suffix):
    scons_target = str(libdep).replace(
        f"{env.Dir('#').abspath}/{env['BAZEL_OUT_DIR']}/src", env.Dir("$BUILD_DIR").path
    )
    bazel_target = env["SCONS2BAZEL_TARGETS"].bazel_target(scons_target)
    bazel_libdep = env.File(f"#/{env['SCONS2BAZEL_TARGETS'].bazel_output(scons_target)}")

    query_results = env.CheckBazelDepsCache(bazel_target)

    if query_results is None:
        linkfile = env["SCONS2BAZEL_TARGETS"].bazel_link_file(scons_target)
        with open(os.path.join(env.Dir("#").abspath, linkfile)) as f:
            query_results = f.read()

        filtered_results = ""
        for lib in query_results.splitlines():
            bazel_out_path = lib.replace(f"{env['BAZEL_OUT_DIR']}/src", "bazel-bin/src")
            if os.path.exists(env.File("#/" + bazel_out_path + ".exclude_lib").abspath):
                continue
            filtered_results += lib + "\n"
        query_results = filtered_results

        env.AddBazelDepsCache(bazel_target, query_results)

    for line in query_results.splitlines():
        # We are only interested in installing shared libs and their debug files
        if not line.endswith(shlib_suffix):
            continue

        bazel_node = env.File(f"#/{line}")
        bazel_node_debug = env.File(f"#/{line}$SEPDBG_SUFFIX")

        setattr(bazel_node_debug.attributes, "debug_file_for", bazel_node)
        setattr(bazel_node.attributes, "separate_debug_files", [bazel_node_debug])

        auto_install_single_target(env, bazel_libdep, shlib_suffix, bazel_node)

        if env.GetAutoInstalledFiles(bazel_libdep):
            auto_install_single_target(
                env,
                getattr(bazel_libdep.attributes, "separate_debug_files")[0],
                env.subst("$SEPDBG_SUFFIX"),
                bazel_node_debug,
            )

    return env.GetAutoInstalledFiles(libdep)


def auto_archive_bazel(env, node, already_archived, search_stack):
    bazel_child = getattr(node.attributes, "AIB_INSTALL_FROM", node)
    if not str(bazel_child).startswith("bazel-out"):
        try:
            bazel_child = env["SCONS2BAZEL_TARGETS"].bazel_output(bazel_child.path)
        except KeyError:
            return

    if str(bazel_child) not in already_archived:
        already_archived.add(str(bazel_child))
        scons_target = str(bazel_child).replace(
            f"{env['BAZEL_OUT_DIR']}/src", env.Dir("$BUILD_DIR").path
        )
        linkfile = env["SCONS2BAZEL_TARGETS"].bazel_link_file(scons_target)

        with open(os.path.join(env.Dir("#").abspath, linkfile)) as f:
            query_results = f.read()

        filtered_results = ""
        for lib in query_results.splitlines():
            bazel_out_path = lib.replace("\\", "/").replace(
                f"{env['BAZEL_OUT_DIR']}/src", "bazel-bin/src"
            )
            if os.path.exists(
                env.File("#/" + bazel_out_path + ".exclude_lib").abspath.replace("\\", "/")
            ):
                continue
            filtered_results += lib + "\n"
        query_results = filtered_results
        for lib in query_results.splitlines():
            if str(bazel_child).endswith(env.subst("$SEPDBG_SUFFIX")):
                debug_file = getattr(env.File("#/" + lib).attributes, "separate_debug_files")[0]
                bazel_install_file = env.GetAutoInstalledFiles(debug_file)[0]
            else:
                bazel_install_file = env.GetAutoInstalledFiles(env.File("#/" + lib))[0]

            if bazel_install_file:
                search_stack.append(bazel_install_file)


def load_bazel_builders(env):
    # === Builders ===
    create_program_builder(env)

    if env.GetOption("ninja") != "disabled":
        env.NinjaRule(
            "BAZEL_COPY_RULE", "$env$cmd", description="Copy from Bazel", pool="local_pool"
        )


total_libdeps_linking_time = 0
count_of_libdeps_links = 0


def add_libdeps_time(env, delate_time):
    global total_libdeps_linking_time, count_of_libdeps_links
    total_libdeps_linking_time += delate_time
    count_of_libdeps_links += 1


def prefetch_toolchain(env):
    setup_bazel_env_vars()
    setup_max_retry_attempts()
    bazel_bin_dir = (
        env.GetOption("evergreen-tmp-dir")
        if env.GetOption("evergreen-tmp-dir")
        else os.path.expanduser("~/.local/bin")
    )
    if not os.path.exists(bazel_bin_dir):
        os.makedirs(bazel_bin_dir)
    Globals.bazel_executable = install_bazel(bazel_bin_dir)
    if platform.system() == "Linux" and not ARGUMENTS.get("CC") and not ARGUMENTS.get("CXX"):
        exec_root = f'bazel-{os.path.basename(env.Dir("#").abspath)}'
        if exec_root and not os.path.exists(f"{exec_root}/external/mongo_toolchain"):
            print("Prefetch the mongo toolchain...")
            try:
                retry_call(
                    subprocess.run,
                    [[Globals.bazel_executable, "build", "@mongo_toolchain", "--config=local"]],
                    fkwargs={
                        "env": {**os.environ.copy(), **Globals.bazel_env_variables},
                        "check": True,
                    },
                    tries=Globals.max_retry_attempts,
                    exceptions=(subprocess.CalledProcessError,),
                )
            except subprocess.CalledProcessError as ex:
                print("ERROR: Bazel fetch failed!")
                print(ex)
                print("Please ask about this in #ask-devprod-build slack channel.")
                sys.exit(1)

        return exec_root


# Required boilerplate function
def exists(env: SCons.Environment.Environment) -> bool:
    # === Bazelisk ===

    write_workstation_bazelrc()
    env.AddMethod(prefetch_toolchain, "PrefetchToolchain")
    env.AddMethod(load_bazel_builders, "LoadBazelBuilders")
    return True


def handle_bazel_program_exception(env, target, outputs):
    prog_suf = env.subst("$PROGSUFFIX")
    dbg_suffix = ".pdb" if sys.platform == "win32" else env.subst("$SEPDBG_SUFFIX")
    bazel_program = False

    # on windows the pdb for dlls contains no double extensions
    # so we need to check all the outputs up front to know
    for bazel_output_file in outputs:
        if bazel_output_file.endswith(".dll"):
            return False

    if os.path.splitext(outputs[0])[1] in [prog_suf, dbg_suffix]:
        for bazel_output_file in outputs:
            first_ext = os.path.splitext(bazel_output_file)[1]
            if dbg_suffix and first_ext == dbg_suffix:
                second_ext = os.path.splitext(os.path.splitext(bazel_output_file)[0])[1]
            else:
                second_ext = None

            if (
                (second_ext is not None and second_ext + first_ext == prog_suf + dbg_suffix)
                or (second_ext is None and first_ext == prog_suf)
                or first_ext == ".exe"
                or first_ext == ".pdb"
            ):
                bazel_program = True
                scons_node_str = bazel_output_file.replace(
                    f"{env['BAZEL_OUT_DIR']}/src", env.Dir("$BUILD_DIR").path.replace("\\", "/")
                )

                Globals.scons2bazel_targets[scons_node_str.replace("\\", "/")] = {
                    "bazel_target": target,
                    "bazel_output": bazel_output_file.replace("\\", "/"),
                }
    return bazel_program


def write_workstation_bazelrc():
    if os.environ.get("CI") is None:
        workstation_file = ".bazelrc.workstation"
        existing_hash = ""
        if os.path.exists(workstation_file):
            with open(workstation_file) as f:
                existing_hash = hashlib.md5(f.read().encode()).hexdigest()

        try:
            repo = git.Repo()
        except Exception:
            print(
                "Unable to setup git repo, skipping workstation file generation. This will result in incomplete telemetry data being uploaded."
            )
            return

        try:
            status = "clean" if repo.head.commit.diff(None) is None else "modified"
        except Exception:
            status = "Unknown"

        try:
            hostname = socket.gethostname()
        except Exception:
            hostname = "Unknown"

        try:
            remote = repo.branches.master.repo.remote().url
        except Exception:
            try:
                remote = repo.remotes[0].url
            except Exception:
                remote = "Unknown"

        try:
            branch = repo.active_branch.name
        except Exception:
            branch = "Unknown"

        try:
            commit = repo.commit("HEAD")
        except Exception:
            commit = "Unknown"

        bazelrc_contents = f"""\
# Generated file, do not modify
common --bes_keywords=developerBuild=True
common --bes_keywords=workstation={hostname}
common --bes_keywords=engflow:BuildScmRemote={remote}
common --bes_keywords=engflow:BuildScmBranch={branch}
common --bes_keywords=engflow:BuildScmRevision={commit}
common --bes_keywords=engflow:BuildScmStatus={status}
    """

        current_hash = hashlib.md5(bazelrc_contents.encode()).hexdigest()
        if existing_hash != current_hash:
            print(f"Generating new {workstation_file} file...")
            with open(workstation_file, "w") as f:
                f.write(bazelrc_contents)


def setup_bazel_env_vars() -> None:
    # Set the JAVA_HOME directories for ppc64le and s390x since their bazel binaries are not compiled with a built-in JDK.
    if platform.machine().lower() in {"ppc64le", "s390x"}:
        Globals.bazel_env_variables["JAVA_HOME"] = "/usr/lib/jvm/java-21-openjdk"


def setup_max_retry_attempts() -> None:
    Globals.max_retry_attempts = (
        _CI_MAX_RETRY_ATTEMPTS if os.environ.get("CI") is not None else _LOCAL_MAX_RETRY_ATTEMPTS
    )


def is_local_execution(env: SCons.Environment.Environment) -> bool:
    normalized_arch = (
        platform.machine().lower().replace("aarch64", "arm64").replace("x86_64", "amd64")
    )
    user_flags = shlex.split(env.get("BAZEL_FLAGS", ""))
    return (
        os.environ.get("USE_NATIVE_TOOLCHAIN")
        or normalized_arch not in ["arm64", "amd64"]
        or "--config=local" in user_flags
        or "--config=public-release" in user_flags
    )


def generate(env: SCons.Environment.Environment) -> None:
    if env["BAZEL_INTEGRATION_DEBUG"]:
        global bazel_debug

        def bazel_debug_func(msg: str):
            print("[BAZEL_INTEGRATION_DEBUG] " + str(msg))

        bazel_debug = bazel_debug_func

    # this should be populated from the sconscript and include list of targets scons
    # indicates it wants to build
    env["SCONS_SELECTED_TARGETS"] = []

    # === Architecture/platform ===

    # Bail if current architecture not supported for Bazel:
    normalized_arch = (
        platform.machine().lower().replace("aarch64", "arm64").replace("x86_64", "amd64")
    )
    normalized_os = sys.platform.replace("win32", "windows").replace("darwin", "macos")
    current_platform = f"{normalized_os}:{normalized_arch}:{env.ToolchainName()}"
    if current_platform not in _SUPPORTED_PLATFORM_MATRIX:
        raise Exception(
            f'Bazel not supported on this platform ({current_platform}); supported platforms are: [{", ".join(_SUPPORTED_PLATFORM_MATRIX)}]'
        )

    # === Build settings ===

    # We don't support DLL generation on Windows, but need shared object generation in dynamic-sdk mode
    # on linux.
    linkstatic = env.GetOption("link-model") in ["auto", "static"] or (
        normalized_os == "windows" and env.GetOption("link-model") == "dynamic-sdk"
    )

    allocator = env.get("MONGO_ALLOCATOR", "tcmalloc-google")

    distro_or_os = normalized_os
    if normalized_os == "linux":
        distro_id = find_distro_match(f"{distro.name()} {distro.version()}")
        if distro_id is not None:
            distro_or_os = distro_id

    bazel_internal_flags = [
        f"--compiler_type={env.ToolchainName()}",
        f'--opt={env.GetOption("opt")}',
        f'--dbg={env.GetOption("dbg") == "on"}',
        f'--debug_symbols={env.GetOption("debug-symbols") == "on"}',
        f'--thin_lto={env.GetOption("thin-lto") is not None}',
        f'--separate_debug={True if env.GetOption("separate-debug") == "on" else False}',
        f'--libunwind={env.GetOption("use-libunwind")}',
        f'--use_gdbserver={False if env.GetOption("gdbserver") is None else True}',
        f'--spider_monkey_dbg={True if env.GetOption("spider-monkey-dbg") == "on" else False}',
        f"--allocator={allocator}",
        f'--use_lldbserver={False if env.GetOption("lldb-server") is None else True}',
        f'--use_wait_for_debugger={False if env.GetOption("wait-for-debugger") is None else True}',
        f'--use_ocsp_stapling={True if env.GetOption("ocsp-stapling") == "on" else False}',
        f'--use_disable_ref_track={False if env.GetOption("disable-ref-track") is None else True}',
        f'--use_wiredtiger={True if env.GetOption("wiredtiger") == "on" else False}',
        f'--use_glibcxx_debug={env.GetOption("use-glibcxx-debug") is not None}',
        f'--use_tracing_profiler={env.GetOption("use-tracing-profiler") == "on"}',
        f'--build_grpc={True if env["ENABLE_GRPC_BUILD"] else False}',
        f'--use_libcxx={env.GetOption("libc++") is not None}',
        f'--detect_odr_violations={env.GetOption("detect-odr-violations") is not None}',
        f"--linkstatic={linkstatic}",
        f'--shared_archive={env.GetOption("link-model") == "dynamic-sdk"}',
        f'--linker={env.GetOption("linker")}',
        f'--streams_release_build={env.GetOption("streams-release-build")}',
        f'--release={env.GetOption("release") == "on"}',
        f'--build_enterprise={"MONGO_ENTERPRISE_VERSION" in env}',
        f'--visibility_support={env.GetOption("visibility-support")}',
        f'--disable_warnings_as_errors={"source" in env.GetOption("disable-warnings-as-errors")}',
        f'--gcov={env.GetOption("gcov") is not None}',
        f'--pgo_profile={env.GetOption("pgo-profile") is not None}',
        f'--server_js={env.GetOption("server-js") == "on"}',
        f'--ssl={"True" if env.GetOption("ssl") == "on" else "False"}',
        f'--js_engine={env.GetOption("js-engine")}',
        f'--use_sasl_client={env.GetOption("use-sasl-client") is not None}',
        "--define",
        f"MONGO_VERSION={env['MONGO_VERSION']}",
        "--define",
        f"MONGO_DISTMOD={env['MONGO_DISTMOD']}",
        "--compilation_mode=dbg",  # always build this compilation mode as we always build with -g
        "--dynamic_mode=off",
    ]

    # Timeout linking on windows at 5 minutes to retry with a lower concurrency.
    if platform.system() == "Windows":
        bazel_internal_flags += [
            "--link_timeout_8min=True",
        ]

    if not os.environ.get("USE_NATIVE_TOOLCHAIN"):
        bazel_internal_flags += [
            f"--platforms=//bazel/platforms:{distro_or_os}_{normalized_arch}",
            f"--host_platform=//bazel/platforms:{distro_or_os}_{normalized_arch}",
        ]

    if "MONGO_ENTERPRISE_VERSION" in env:
        enterprise_features = env.GetOption("enterprise_features")
        if enterprise_features == "*":
            bazel_internal_flags += ["--//bazel/config:enterprise_feature_all=True"]
        else:
            bazel_internal_flags += ["--//bazel/config:enterprise_feature_all=False"]
            bazel_internal_flags += [
                f"--//bazel/config:enterprise_feature_{feature}=True"
                for feature in enterprise_features.split(",")
            ]

    # TODO SERVER-97028
    # remove when ssl disabled builds are fixed
    if env.GetOption("ssl") == "off":
        bazel_internal_flags += ["--keep_going"]

    if env.GetOption("gcov") is not None:
        bazel_internal_flags += ["--collect_code_coverage"]

    if env["DWARF_VERSION"]:
        bazel_internal_flags.append(f"--dwarf_version={env['DWARF_VERSION']}")

    if normalized_os == "macos":
        bazel_internal_flags.append(
            f"--developer_dir={os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app')}"
        )
        minimum_macos_version = "11.0" if normalized_arch == "arm64" else "10.14"
        bazel_internal_flags.append(f"--macos_minimum_os={minimum_macos_version}")

    if normalized_os == "windows":
        windows_temp_dir = "Z:/bazel_tmp"
        if os.path.isdir(windows_temp_dir):
            bazel_internal_flags.append(f"--action_env=TMP={windows_temp_dir}")
            bazel_internal_flags.append(f"--action_env=TEMP={windows_temp_dir}")
        else:
            print(
                f"Tried to use {windows_temp_dir} as TMP and TEMP environment variables but it did not exist. This will lead to a low cache hit rate."
            )

    http_client_option = env.GetOption("enable-http-client")
    if http_client_option is not None:
        if http_client_option in ["on", "auto"]:
            bazel_internal_flags.append("--http_client=True")
        elif http_client_option == "off":
            bazel_internal_flags.append("--http_client=False")

    sanitizer_option = env.GetOption("sanitize")

    if sanitizer_option is not None and sanitizer_option != "":
        options = sanitizer_option.split(",")
        formatted_options = [f"--{_SANITIZER_MAP[opt]}=True" for opt in options]
        bazel_internal_flags.extend(formatted_options)

    if normalized_arch not in ["arm64", "amd64"]:
        bazel_internal_flags.append("--config=no-remote-exec")
    elif os.environ.get("USE_NATIVE_TOOLCHAIN"):
        print("Custom toolchain detected, using --config=local for bazel build.")
        bazel_internal_flags.append("--config=local")

    if normalized_arch == "s390x":
        # s390x systems don't have enough RAM to handle the default job count and will
        # OOM unless we reduce it.
        bazel_internal_flags.append("--jobs=3")
    elif normalized_arch == "ppc64le":
        # ppc64le builds are OOMing with default concurrency, but it's not clear if it's
        # an issue with the bazel client itself or in the compiler.
        bazel_internal_flags.append("--jobs=32")

    public_release = False
    # Disable remote execution for public release builds.
    if (
        env.GetOption("release") == "on"
        and env.GetOption("remote-exec-release") == "off"
        and (
            env.GetOption("cache-dir") is None
            or env.GetOption("cache-dir") == "$BUILD_ROOT/scons/cache"
        )
    ):
        bazel_internal_flags.append("--config=public-release")
        public_release = True

    evergreen_tmp_dir = env.GetOption("evergreen-tmp-dir")
    if normalized_os == "macos" and evergreen_tmp_dir:
        bazel_internal_flags.append(f"--sandbox_writable_path={evergreen_tmp_dir}")

    setup_bazel_env_vars()
    setup_max_retry_attempts()

    if not is_local_execution(env) and not public_release:
        if not validate_remote_execution_certs(env):
            sys.exit(1)

        if env.GetOption("bazel-dynamic-execution"):
            try:
                docker_detected = (
                    subprocess.run(["docker", "info"], capture_output=True).returncode == 0
                )
            except Exception:
                docker_detected = False
            try:
                podman_detected = (
                    subprocess.run(["podman", "--help"], capture_output=True).returncode == 0
                )
            except Exception:
                podman_detected = False

            if not docker_detected:
                print("Not using dynamic scheduling because docker not detected ('docker info').")
            elif docker_detected and podman_detected:
                print(
                    "Docker and podman detected, disabling dynamic scheduling due to uncertainty in docker setup."
                )
            else:
                # TODO: SERVER-95737 fix docker issues on ubuntu24
                if distro_or_os == "ubuntu24":
                    print("Ubuntu24 is not supported to with dynamic scheduling. See SERVER-95737")
                else:
                    remote_execution_containers = {}
                    container_file_path = "bazel/platforms/remote_execution_containers.bzl"
                    with open(container_file_path, "r") as f:
                        code = compile(f.read(), container_file_path, "exec")
                        exec(code, {}, remote_execution_containers)

                    docker_image = remote_execution_containers["REMOTE_EXECUTION_CONTAINERS"][
                        f"{distro_or_os}"
                    ]["container-url"]

                    jobs = int(psutil.cpu_count() * 2) if os.environ.get("CI") else 400

                    bazel_internal_flags += [
                        "--experimental_enable_docker_sandbox",
                        f"--experimental_docker_image={docker_image}",
                        "--experimental_docker_use_customized_images",
                        "--internal_spawn_scheduler",
                        "--dynamic_local_strategy=docker",
                        "--spawn_strategy=dynamic",
                        f"--jobs={jobs}",
                    ]

    Globals.bazel_base_build_command = (
        [
            os.path.abspath(Globals.bazel_executable),
            "build",
        ]
        + bazel_internal_flags
        + shlex.split(env.get("BAZEL_FLAGS", ""))
    )

    log_dir = env.Dir("$BUILD_ROOT/scons/bazel").path
    os.makedirs(log_dir, exist_ok=True)
    with open(os.path.join(log_dir, "bazel_command"), "w") as f:
        f.write(" ".join(Globals.bazel_base_build_command))

    # Store the bazel command line flags so scons can check if it should rerun the bazel targets
    # if the bazel command line changes.
    env["BAZEL_FLAGS_STR"] = bazel_internal_flags + shlex.split(env.get("BAZEL_FLAGS", ""))

    # We always use --compilation_mode debug for now as we always want -g, so assume -dbg location
    out_dir_platform = "$TARGET_ARCH"
    if normalized_os == "macos":
        out_dir_platform = "darwin_arm64" if normalized_arch == "arm64" else "darwin"
    elif normalized_os == "windows":
        out_dir_platform = "x64_windows"
    elif normalized_os == "linux" and normalized_arch == "amd64":
        # For c++ toolchains, bazel has some wierd behaviour where it thinks the default
        # cpu is "k8" which is another name for x86_64 cpus, so its not wrong, but abnormal
        out_dir_platform = "k8"
    elif normalized_arch == "ppc64le":
        out_dir_platform = "ppc"

    env["BAZEL_OUT_DIR"] = env.Dir(f"#/bazel-out/{out_dir_platform}-dbg/bin/").path.replace(
        "\\", "/"
    )

    if env.get("__NINJA_NO") == "1":
        return

    # ThinTarget builder is a special bazel target and should not be prefixed with Bazel in the builder
    # name to exclude it from the other BazelBuilder's. This builder excludes any normal builder
    # mechanisms like scanners or emitters and functions as a pass through for targets which exist
    # only in bazel. It contains no dependency information and is not meant to fully function within
    # the scons dependency graph.
    env["BUILDERS"]["ThinTarget"] = SCons.Builder.Builder(
        action=BazelCopyOutputsAction,
        emitter=SCons.Builder.ListEmitter([bazel_target_emitter]),
    )

    cmd = (
        ["aquery"]
        + env["BAZEL_FLAGS_STR"]
        + [
            'mnemonic("StripDebuginfo|ExtractDebuginfo|Symlink|IdlcGenerator|TemplateRenderer", (outputs("bazel-out/.*/bin/src/.*", deps(@//src/...))))'
        ]
    )

    try:
        results = retry_call(
            bazel_query_func,
            [env, cmd.copy(), "discover ThinTargets"],
            tries=Globals.max_retry_attempts,
            exceptions=(subprocess.CalledProcessError,),
        )
    except subprocess.CalledProcessError as ex:
        print("ERROR: bazel thin targets query failed:")
        print(ex)
        print("Please ask about this in #ask-devprod-build slack channel.")
        sys.exit(1)

    for action in results.stdout.split("\n\n"):
        action = action.strip()
        if not action:
            continue

        lines = action.splitlines()
        bazel_program = False
        for line in lines:
            if line.startswith("  Target: "):
                target = line.replace("  Target: ", "").strip()

            if line.startswith("  Outputs: ["):
                outputs = [
                    line.strip()
                    for line in line.replace("  Outputs: [", "").replace("]", "").strip().split(",")
                ]

                # TODO when we support test lists in bazel we can make BazelPrograms thin targets
                bazel_program = handle_bazel_program_exception(env, target, outputs)

        scons_node_strs = [
            bazel_output_file.replace(
                f"{env['BAZEL_OUT_DIR']}/src", env.Dir("$BUILD_DIR").path.replace("\\", "/")
            )
            for bazel_output_file in outputs
        ]

        if bazel_program:
            for scons_node, bazel_output_file in zip(scons_node_strs, outputs):
                Globals.scons2bazel_targets[scons_node.replace("\\", "/")] = {
                    "bazel_target": target,
                    "bazel_output": bazel_output_file.replace("\\", "/"),
                }
            continue

        scons_nodes = env.ThinTarget(
            target=scons_node_strs, source=outputs, NINJA_GENSOURCE_INDEPENDENT=True
        )
        env.NoCache(scons_nodes)

        for scons_node, bazel_output_file in zip(scons_nodes, outputs):
            Globals.scons2bazel_targets[scons_node.path.replace("\\", "/")] = {
                "bazel_target": target,
                "bazel_output": bazel_output_file.replace("\\", "/"),
            }

    globals = Globals()
    env["SCONS2BAZEL_TARGETS"] = globals

    def print_total_query_time():
        global total_query_time, total_queries
        global time_auto_installing, count_of_auto_installing
        global total_libdeps_linking_time, count_of_libdeps_links
        bazel_debug(
            f"Bazel integration spent {total_query_time} seconds in total performing {total_queries} queries."
        )
        bazel_debug(
            f"Bazel integration spent {time_auto_installing} seconds in total performing {count_of_auto_installing} auto_install."
        )
        bazel_debug(
            f"Bazel integration spent {total_libdeps_linking_time} seconds in total performing {count_of_libdeps_links} libdeps linking."
        )

    atexit.register(print_total_query_time)

    load_bazel_builders(env)
    bazel_build_thread = threading.Thread(
        target=bazel_build_thread_func,
        args=(env, log_dir, env["VERBOSE"], env.GetOption("ninja") != "disabled"),
    )
    bazel_build_thread.start()

    def wait_for_bazel(env):
        nonlocal bazel_build_thread
        Globals.waiting_on_bazel_flag = True
        print("SCons done, switching to bazel build thread...")
        bazel_build_thread.join()
        if Globals.bazel_thread_terminal_output is not None:
            Globals.bazel_thread_terminal_output.seek(0)
            sys.stdout.write(Globals.bazel_thread_terminal_output.read())
        if not Globals.bazel_build_success:
            raise SCons.Errors.BuildError(
                errstr=f"Bazel Build failed with {Globals.bazel_build_exitcode}!",
                status=Globals.bazel_build_exitcode,
                exitstatus=1,
            )

    env.AddMethod(wait_for_bazel, "WaitForBazel")

    env.AddMethod(run_bazel_command, "RunBazelCommand")
    env.AddMethod(add_libdeps_time, "AddLibdepsTime")
    env.AddMethod(generate_bazel_info_for_ninja, "GenerateBazelInfoForNinja")
    env.AddMethod(bazel_deps_check_query_cache, "CheckBazelDepsCache")
    env.AddMethod(bazel_deps_add_query_cache, "AddBazelDepsCache")
    env.AddMethod(bazel_deps_check_link_query_cache, "CheckBazelLinkDepsCache")
    env.AddMethod(bazel_deps_add_link_query_cache, "AddBazelLinkDepsCache")
    env.AddMethod(bazel_query_func, "RunBazelQuery")
    env.AddMethod(ninja_bazel_builder, "NinjaBazelBuilder")
    env.AddMethod(auto_install_bazel, "BazelAutoInstall")
    env.AddMethod(auto_install_single_target, "BazelAutoInstallSingleTarget")
    env.AddMethod(auto_archive_bazel, "BazelAutoArchive")
