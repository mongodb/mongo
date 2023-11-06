import atexit
import functools
import os
import platform
import queue
import shlex
import shutil
import stat
import subprocess
import threading
import time
from typing import List, Dict, Set, Tuple
import urllib.request

import SCons

import mongo.generators as mongo_generators


class Globals:

    # key: scons target, value: bazel target
    scons2bazel_targets: Dict[str, str] = dict()

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
    # bazel uses source paths in the output i.e.: src/mongo/db
    bazel_dir = os.path.dirname(bazel_path)

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

    # sometimes there are multiple outputs, here we are mapping all the
    # bazel outputs to the first scons output as the key. Later the targets
    # will be lined up for the copy.
    Globals.scons_output_to_bazel_outputs[target[0]] = []

    # only the first target in a multi target node will represent the nodes
    Globals.scons2bazel_targets[target[0].abspath] = convert_scons_node_to_bazel_target(target[0])

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
        Globals.scons_output_to_bazel_outputs[target[0]] += [bazel_out_target]

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

    bazel_target = Globals.scons2bazel_targets[target[0].abspath]
    bazel_debug(f"Checking if {bazel_target} is done...")

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
    for s, t in zip(Globals.scons_output_to_bazel_outputs[target[0]], target):
        bazel_debug(f"Copying {s} from bazel tree to {t} in the scons tree.")
        shutil.copyfile(s, str(t))


BazelCopyOutputsAction = SCons.Action.FunctionAction(
    bazel_builder_action,
    {"cmdstr": "Asking bazel to build $TARGET", "varlist": ['BAZEL_FLAGS_STR']},
)


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


def create_bazel_builder(builder):
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
        current_architecture = platform.machine()
        supported_architectures = ['aarch64']
        if current_architecture not in supported_architectures:
            raise Exception(
                f'Bazel not supported on this architecture ({current_architecture}); supported architectures are: [{supported_architectures}]'
            )

        if not env.ToolchainIs('gcc', 'clang'):
            raise Exception(
                f"Unsupported Bazel c++ toolchain: {env.ToolchainName()} is neither `gcc` nor `clang`"
            )

        # === Bazelisk ===

        # TODO(SERVER-81038): remove once bazel/bazelisk is self-hosted.
        if not os.path.exists("bazelisk"):
            urllib.request.urlretrieve(
                "https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-linux-arm64",
                "bazelisk")
            os.chmod("bazelisk", stat.S_IXUSR)

        # === Build settings ===

        static_link = env.GetOption("link-model") in ["auto", "static"]
        if not env.ToolchainIs('gcc', 'clang'):
            raise Exception(f"Toolchain {env.ToolchainName()} is neither `gcc` nor `clang`")

        if env.GetOption("release") is not None:
            build_mode = "release"
        elif env.GetOption("dbg") == "on":
            build_mode = "dbg"
        else:
            build_mode = f"opt_{mongo_generators.get_opt_options(env)}"  # one of "on", "size", "debug"

        bazel_internal_flags = [
            f'--//bazel/config:compiler_type={env.ToolchainName()}',
            f'--//bazel/config:build_mode={build_mode}',
            f'--//bazel/config:use_libunwind={env["USE_VENDORED_LIBUNWIND"]}',
            f'--//bazel/config:use_gdbserver={False if env.GetOption("gdbserver") is None else True}',
            '--compilation_mode=dbg',  # always build this compilation mode as we always build with -g
            '--dynamic_mode=%s' % ('off' if static_link else 'fully'),
        ]

        Globals.bazel_base_build_command = [
            os.path.abspath("bazelisk"),
            'build',
        ] + bazel_internal_flags + shlex.split(env.get("BAZEL_FLAGS", ""))

        # Store the bazel command line flags so scons can check if it should rerun the bazel targets
        # if the bazel command line changes.
        env['BAZEL_FLAGS_STR'] = str(bazel_internal_flags) + env.get("BAZEL_FLAGS", "")

        # We always use --compilation_mode debug for now as we always want -g, so assume -dbg location
        env["BAZEL_OUT_DIR"] = env.Dir("#/bazel-out/$TARGET_ARCH-dbg/bin/")

        # === Builders ===
        create_library_builder(env)
        create_program_builder(env)

        def shutdown_bazel_builer():
            Globals.kill_bazel_thread_flag = True

        atexit.register(shutdown_bazel_builer)

        bazel_build_thread = threading.Thread(target=bazel_batch_build_thread,
                                              args=(env.Dir("$BUILD_ROOT/scons/bazel").path, ))
        bazel_build_thread.daemon = True
        bazel_build_thread.start()
    else:
        env['BUILDERS']['BazelLibrary'] = env['BUILDERS']['Library']
        env['BUILDERS']['BazelProgram'] = env['BUILDERS']['Program']
