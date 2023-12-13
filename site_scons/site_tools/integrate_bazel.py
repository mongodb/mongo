import atexit
import functools
import json
import os
import platform
import queue
import shlex
import shutil
import stat
import subprocess
import threading
import time
from typing import List, Dict, Set, Tuple, Any
import urllib.request
import sys

import SCons

import mongo.platform as mongo_platform
import mongo.generators as mongo_generators

_SUPPORTED_PLATFORM_MATRIX = [
    "linux:arm64:gcc",
    "linux:arm64:clang",
    "windows:amd64:msvc",
    "macos:amd64:clang",
    "macos:arm64:clang",
]


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

    # Flag to signal that the bazel build thread should die and/or is not running
    kill_bazel_thread_flag: bool = False


def bazel_debug(msg: str):
    pass


# Required boilerplate function
def exists(env: SCons.Environment.Environment) -> bool:
    return True


def convert_scons_node_to_bazel_target(scons_node: SCons.Node.FS.File) -> str:
    """Convert a scons node object into a bazel target label."""

    # gets the SCons.Environment for the node
    env = scons_node.get_env()

    # convert to the source path i.e.: src/mongo/db/libcommands.so
    bazel_path = scons_node.srcnode().path
    # bazel uses source paths in the output i.e.: src/mongo/db, replace backslashes on windows
    bazel_dir = os.path.dirname(bazel_path).replace("\\", "/")

    # extract the platform prefix for a given file so we can remove it i.e.: libcommands.so -> 'lib'
    prefix = env.subst(scons_node.get_builder().get_prefix(env), target=[scons_node],
                       source=scons_node.sources) if scons_node.has_builder() else ""

    # now get just the file name without and prefix or suffix i.e.: libcommands.so -> 'commands'
    prefix_suffix_removed = os.path.splitext(scons_node.name[len(prefix):])[0]

    # i.e.: //src/mongo/db:commands>
    return f"//{bazel_dir}:{prefix_suffix_removed}"


def bazel_target_emitter(
        target: List[SCons.Node.Node], source: List[SCons.Node.Node],
        env: SCons.Environment.Environment) -> Tuple[List[SCons.Node.Node], List[SCons.Node.Node]]:
    """This emitter will map any scons outputs to bazel outputs so copy can be done later."""

    for t in target:

        # normally scons emitters conveniently build-ify the target paths so it will
        # reference the output location, but we actually want the node path
        # from the original source tree location, so srcnode() will do this for us
        bazel_path = t.srcnode().path
        bazel_dir = os.path.dirname(bazel_path)

        # the new builders are just going to copy, so we are going to calculate the bazel
        # output location and then set that as the new source for the builders.
        bazel_out_dir = env.get("BAZEL_OUT_DIR")
        bazel_out_target = f'{bazel_out_dir}/{bazel_dir}/{os.path.basename(bazel_path)}'

        Globals.scons2bazel_targets[t.path.replace('\\', '/')] = {
            'bazel_target': convert_scons_node_to_bazel_target(t),
            'bazel_output': bazel_out_target.replace('\\', '/')
        }

    return (target, source)


def bazel_builder_action(env: SCons.Environment.Environment, target: List[SCons.Node.Node],
                         source: List[SCons.Node.Node]):
    def check_bazel_target_done(bazel_target: str) -> bool:
        """
        Check the done queue and pull off the desired target if it exists.
        
        bazel_target: the target we are looking for
        return: True to stop waiting, False if we should keep waiting 

        Note that this function will signal True to indicate a shutdown/failure case. SCons main build
        thread will be waiting for this action to complete before shutting down, so we need to
        to stop being blocked so scons will proceed to shutdown.
        """

        if bazel_target in Globals.bazel_targets_done:
            bazel_debug(f"Removing {bazel_target} from done targets: {Globals.bazel_targets_done}")
            Globals.bazel_targets_done.remove(bazel_target)
            return True

        # Because of the tight while loop this function is used in, we put
        # an exit condition here.
        if Globals.kill_bazel_thread_flag:
            return True

        return False

    bazel_output = Globals.scons2bazel_targets[target[0].path.replace('\\', '/')]['bazel_output']
    bazel_target = Globals.scons2bazel_targets[target[0].path.replace('\\', '/')]['bazel_target']
    bazel_debug(f"Checking if {bazel_output} is done...")

    # put the target into the work queue the poll until its
    # been placed into the done queue
    bazel_debug(f"A builder put {bazel_target} into the work queue.")
    pred = functools.partial(check_bazel_target_done, bazel_target)
    start_time = time.time()
    Globals.bazel_targets_work_queue.put(bazel_target)

    bazel_debug(f"Waiting to remove {bazel_target} from done targets.")
    with Globals.bazel_target_done_CV:
        Globals.bazel_target_done_CV.wait_for(predicate=pred)

    if Globals.kill_bazel_thread_flag:
        # scons expects actions to return non-zero value on failure
        return 1

    bazel_debug(
        f"Bazel done with {bazel_target} in {'{0:.2f}'.format(time.time() - start_time)} seconds.")

    # now copy all the targets out to the scons tree, note that target is a
    # list of nodes so we need to stringify it for copyfile
    for t in target:
        s = Globals.scons2bazel_targets[t.path.replace('\\', '/')]['bazel_output']
        bazel_debug(f"Copying {s} from bazel tree to {t} in the scons tree.")
        shutil.copyfile(s, str(t))


BazelCopyOutputsAction = SCons.Action.FunctionAction(
    bazel_builder_action,
    {"cmdstr": "Asking bazel to build $TARGET", "varlist": ['BAZEL_FLAGS_STR']},
)


# the ninja tool has some API that doesn't support using SCons env methods
# instead of adding more API to the ninja tool which has a short life left
# we just add the unused arg _dup_env
def ninja_bazel_builder(env: SCons.Environment.Environment, _dup_env: SCons.Environment.Environment,
                        node: SCons.Node.Node) -> Dict[str, Any]:
    """
    Translator for ninja which turns the scons bazel_builder_action
    into a build node that ninja can digest.
    """

    outs = env.NinjaGetOutputs(node)
    ins = [Globals.scons2bazel_targets[out.replace('\\', '/')]['bazel_output'] for out in outs]

    # this represents the values the ninja_syntax.py will use to generate to real
    # ninja syntax defined in the ninja manaul: https://ninja-build.org/manual.html#ref_ninja_file
    return {
        "outputs": outs,
        "inputs": ins,
        "rule": "BAZEL_COPY_RULE",
        "variables": {
            "cmd":
                ' & '.join([
                    f"$COPY {input_node.replace('/',os.sep)} {output_node}"
                    for input_node, output_node in zip(ins, outs)
                ])
        },
    }


def bazel_batch_build_thread(log_dir: str) -> None:
    """This thread continuelly runs bazel when ever new targets are found."""

    # the set of targets which bazel has already built.
    bazel_built_targets = set()
    bazel_batch_count = 0

    print_start_time = time.time()
    start_time = time.time()

    os.makedirs(log_dir, exist_ok=True)

    try:
        while not Globals.kill_bazel_thread_flag:

            # SCons must (try to) make sure that all currently running build tasks come to compeletion,
            # so that it can correctly transcribe the state of a given node to the SConsign file.
            # Therefore it takes control over many signals which would end scons process (i.e. SIGINT),
            # we need to make sure that we break out of this loop and free our CVs which the scons
            # main build thread will be waiting on to complete, otherwise that will cause scons to
            # wait forever.
            if SCons.Script.Main.jobs and SCons.Script.Main.jobs.were_interrupted():
                Globals.kill_bazel_thread_flag = True
                bazel_debug("TaskMaster not running, shutting down bazel thread.")
                break

            if time.time() - print_start_time > 10.0:
                print_start_time = time.time()
                bazel_debug(
                    f"Bazel batch thread has built {len(bazel_built_targets)} targets so far.")

            targets_to_build = set()
            while not Globals.bazel_targets_work_queue.empty():
                # this thread should be the only thing pulling off the work queue, so if it was
                # not empty, it must still be not empty when we perform the get. We intend
                # for the get_nowait to raise Empty exception as a hard fail if previously
                # stated assumption was incorrect.
                target = Globals.bazel_targets_work_queue.get_nowait()
                if target not in bazel_built_targets:
                    bazel_debug(f"Bazel batch thread found {target} to build")
                    bazel_built_targets.add(target)
                    targets_to_build.add(target)

            if targets_to_build:
                bazel_debug(
                    f"BAZEL_COMMAND: {Globals.bazel_base_build_command + list(targets_to_build)}")
                start_time = time.time()
                bazel_proc = subprocess.run(
                    Globals.bazel_base_build_command + list(targets_to_build),
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                bazel_debug(
                    f"Bazel build completed in {'{0:.2f}'.format(time.time() - start_time)} seconds and built {targets_to_build}."
                )

                # If we have a failure always print the bazel output, if
                # bazel succeeded to build, only print output with bazel debug
                # on.
                if bazel_proc.returncode:
                    print(bazel_proc.stdout)
                else:
                    bazel_debug(bazel_proc.stdout)

                bazel_batch_count += 1
                with open(os.path.join(log_dir, f"bazel_build_{bazel_batch_count}.log"), "w") as f:
                    f.write(' '.join(Globals.bazel_base_build_command + list(targets_to_build)))
                    f.write(bazel_proc.stdout)

                for t in targets_to_build:
                    # Always put the targets we attempt to build into done
                    # even if bazel failed to build. If bazel failed to build,
                    # scons will report the particular target failed when it goes
                    # to copy it out of the bazel tree.
                    # NOTE: it would be nice if we had a way to bubble up the particular
                    # bazel target log, this would require some way to
                    # parse it from bazel logs.
                    Globals.bazel_targets_done.add(t)
                    bazel_debug(f"Bazel batch thread adding {t} to done queue.")

                with Globals.bazel_target_done_CV:
                    Globals.bazel_target_done_CV.notify_all()

            else:
                time.sleep(1)

    except Exception as exc:
        Globals.kill_bazel_thread_flag = True
        with Globals.bazel_target_done_CV:
            Globals.bazel_target_done_CV.notify_all()
        raise exc


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


# The next section of builders are hook builders. These
# will be standin place holders for the original scons builders, and if bazel build is enabled
# these simply copy out the target from the underlying bazel build
def create_library_builder(env: SCons.Environment.Environment) -> None:
    if env.GetOption("link-model") in ["auto", "static"]:
        env['BUILDERS']['BazelLibrary'] = create_bazel_builder(env['BUILDERS']["StaticLibrary"])
    else:
        env['BUILDERS']['BazelLibrary'] = create_bazel_builder(env['BUILDERS']["SharedLibrary"])


def create_program_builder(env: SCons.Environment.Environment) -> None:
    env['BUILDERS']['BazelProgram'] = create_bazel_builder(env['BUILDERS']["Program"])


def create_idlc_builder(env: SCons.Environment.Environment) -> None:
    env['BUILDERS']['BazelIdlc'] = create_bazel_builder(env['BUILDERS']["Idlc"])


def generate_bazel_info_for_ninja(env: SCons.Environment.Environment) -> None:
    # create a json file which contains all the relevant info from this generation
    # that bazel will need to construct the correct command line for any given targets
    ninja_bazel_build_json = {
        'bazel_cmd': Globals.bazel_base_build_command,
        'defaults': [str(t) for t in SCons.Script.DEFAULT_TARGETS],
        'targets': Globals.scons2bazel_targets
    }
    with open('.bazel_info_for_ninja.txt', 'w') as f:
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
        ninja_bazel_outs += [bazel_t['bazel_output']]
        ninja_bazel_ins += env.NinjaGetInputs(env.File(scons_t))
        if scons_t in ninja_bazel_ins:
            ninja_bazel_ins.remove(scons_t)

    # This is to be used directly by ninja later during generation of the ninja file
    env["NINJA_BAZEL_OUTPUTS"] = ninja_bazel_outs
    env["NINJA_BAZEL_INPUTS"] = ninja_bazel_ins


# Establishes logic for BazelLibrary build rule
def generate(env: SCons.Environment.Environment) -> None:

    if env.get("BAZEL_BUILD_ENABLED"):
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
        normalized_arch = platform.machine().lower().replace("aarch64", "arm64").replace(
            "x86_64", "amd64")
        normalized_os = sys.platform.replace("win32", "windows").replace("darwin", "macos")
        current_platform = f"{normalized_os}:{normalized_arch}:{env.ToolchainName()}"
        if current_platform not in _SUPPORTED_PLATFORM_MATRIX:
            raise Exception(
                f'Bazel not supported on this platform ({current_platform}); supported platforms are: [{", ".join(_SUPPORTED_PLATFORM_MATRIX)}]'
            )

        # === Bazelisk ===

        # TODO(SERVER-81038): remove once bazel/bazelisk is self-hosted.
        if not os.path.exists("bazelisk"):
            ext = ".exe" if normalized_os == "windows" else ""
            os_str = normalized_os.replace("macos", "darwin")
            urllib.request.urlretrieve(
                f"https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-{os_str}-{normalized_arch}{ext}",
                "bazelisk")
            os.chmod("bazelisk", stat.S_IXUSR)

        # === Build settings ===

        static_link = env.GetOption("link-model") in ["auto", "static"]

        if env.GetOption("release") is not None:
            build_mode = "release"
        elif env.GetOption("dbg") == "on":
            build_mode = "dbg"
        else:
            build_mode = f"opt_{mongo_generators.get_opt_options(env)}"  # one of "on", "size", "debug"

        # Deprecate tcmalloc-experimental
        allocator = "tcmalloc" if env.GetOption(
            "allocator") == "tcmalloc-experimental" else env.GetOption("allocator")

        bazel_internal_flags = [
            f'--//bazel/config:compiler_type={env.ToolchainName()}',
            f'--//bazel/config:build_mode={build_mode}',
            f'--//bazel/config:use_libunwind={env["USE_VENDORED_LIBUNWIND"]}',
            f'--//bazel/config:use_gdbserver={False if env.GetOption("gdbserver") is None else True}',
            f'--//bazel/config:spider_monkey_dbg={True if env.GetOption("spider-monkey-dbg") == "on" else False}',
            f'--//bazel/config:allocator={allocator}',
            f'--//bazel/config:use_lldbserver={False if env.GetOption("lldb-server") is None else True}',
            f'--//bazel/config:use_wait_for_debugger={False if env.GetOption("wait-for-debugger") is None else True}',
            f'--//bazel/config:use_ocsp_stapling={True if env.GetOption("ocsp-stapling") == "on" else False}',
            f'--//bazel/config:use_disable_ref_track={False if env.GetOption("disable-ref-track") is None else True}',
            f'--dynamic_mode={"off" if static_link else "fully"}',
            f'--platforms=//bazel/platforms:{normalized_os}_{normalized_arch}_{env.ToolchainName()}',
            f'--host_platform=//bazel/platforms:{normalized_os}_{normalized_arch}_{env.ToolchainName()}',
            '--compilation_mode=dbg',  # always build this compilation mode as we always build with -g
        ]

        if normalized_os != "linux" or normalized_arch not in ["arm64", 'amd64']:
            bazel_internal_flags.append('--config=local')

        Globals.bazel_base_build_command = [
            os.path.abspath("bazelisk"),
            'build',
        ] + bazel_internal_flags + shlex.split(env.get("BAZEL_FLAGS", ""))

        # Store the bazel command line flags so scons can check if it should rerun the bazel targets
        # if the bazel command line changes.
        env['BAZEL_FLAGS_STR'] = str(bazel_internal_flags) + env.get("BAZEL_FLAGS", "")

        # We always use --compilation_mode debug for now as we always want -g, so assume -dbg location
        out_dir_platform = "$TARGET_ARCH"
        if normalized_os == "macos":
            out_dir_platform = "darwin_arm64" if normalized_arch == "arm64" else "darwin"
        elif normalized_os == "windows":
            out_dir_platform = "x64_windows"
        env["BAZEL_OUT_DIR"] = env.Dir(f"#/bazel-out/{out_dir_platform}-dbg/bin/")

        # === Builders ===
        create_library_builder(env)
        create_program_builder(env)
        create_idlc_builder(env)

        if env.GetOption('ninja') == "disabled":

            def shutdown_bazel_builer():
                Globals.kill_bazel_thread_flag = True

            atexit.register(shutdown_bazel_builer)

            # ninja will handle the build so do not launch the bazel batch thread
            bazel_build_thread = threading.Thread(target=bazel_batch_build_thread,
                                                  args=(env.Dir("$BUILD_ROOT/scons/bazel").path, ))
            bazel_build_thread.daemon = True
            bazel_build_thread.start()
        else:
            env.NinjaRule("BAZEL_COPY_RULE", "$env$cmd", description="Copy from Bazel",
                          pool="local_pool")

        env.AddMethod(generate_bazel_info_for_ninja, "GenerateBazelInfoForNinja")
        env.AddMethod(ninja_bazel_builder, "NinjaBazelBuilder")
    else:
        env['BUILDERS']['BazelLibrary'] = env['BUILDERS']['Library']
        env['BUILDERS']['BazelProgram'] = env['BUILDERS']['Program']
        env['BUILDERS']['BazelIdlc'] = env['BUILDERS']['Idlc']
