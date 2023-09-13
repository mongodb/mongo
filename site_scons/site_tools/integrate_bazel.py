import os
import platform
import SCons
import stat
import urllib.request

# TODO(SERVER-81038): remove /tmp/ bazel/bazelisk is added to the toolchain.
# Note: The /tmp location is, ironically, temporary. We expect to implement Bazilisk-installation
# as a standard part of the Bazel solution soon.
BAZELISK_PATH = "/tmp/bazelisk"


# Required boilerplate function
def exists(env):
    return True


# Establishes logic for BazelLibrary build rule
def generate(env):
    def bazel_library(env, target, source, *args, **kwargs):
        # Get info about targets & paths.
        # For a target such as 'fsync_locked', the variables would be:
        #   target_to_build:       fsync_locked
        #   cwd:                   /home/ubuntu/mongo/src/mongo/db/commands
        #   bazel_dir:             src/mongo/db/commands
        #   bazel_target:          //src/mongo/db/commands:fsync_locked
        #   bazel_outfile_base:    bazel-bin/src/mongo/db/commands/libfsync_locked
        #   base_dir_from_scons:   build/fast
        #   scons_outfile_relpath: build/fast/mongo/db/commands
        #   scons_outfile_base:    /home/ubuntu/mongo/build/fast/mongo/db/commands/libfsync_locked
        target_to_build = target[0]
        cwd = os.getcwd()
        bazel_dir = os.path.dirname(env.File(os.path.join(cwd, target_to_build)).srcnode().path)
        bazel_target = f"//{bazel_dir}:{target_to_build}"
        bazel_outfile_base = f"bazel-bin/{bazel_dir}/lib{target_to_build}"
        # Note: The IDLCFLAGS are of the form: [..., '--base_dir', 'build/fast', ...]; the logic
        # below fetches the value immediately after "--base_dir".
        base_dir_from_scons = env.Dictionary()['IDLCFLAGS'][
            env.Dictionary()['IDLCFLAGS'].index('--base_dir') + 1]
        scons_outfile_relpath = bazel_dir.replace('src/', f'{base_dir_from_scons}/')
        scons_outfile_base = cwd.replace(bazel_dir, f'{scons_outfile_relpath}/lib{target_to_build}')
        static_link = env.GetOption("link-model") in ["auto", "static"]
        if not env.ToolchainIs('gcc', 'clang'):
            raise Exception(f"Toolchain {env.ToolchainName()} is neither `gcc` nor `clang`")
        compile_args = [f"--//bazel/config:compiler_type={env.ToolchainName()}"]
        link_args = ["--dynamic_mode=off"] if static_link else ["--dynamic_mode=fully"]

        # Craft series of build actions:
        env['BUILDERS']['_BazelBuild'] = SCons.Builder.Builder(action=" && ".join([
            # Build the target via Bazel
            f"{BAZELISK_PATH} build {bazel_target} {' '.join(compile_args)} {' '.join(link_args)}",

            # Colocate the Bazel static library with the SCons output:
            f"cp -f {bazel_outfile_base}.a {scons_outfile_base}.a",

            # Colocate the Bazel dynamic library with the SCons output:
            f"cp -f {bazel_outfile_base}.so {scons_outfile_base}.so"
        ]))

        # Invoke the build (note that we only specify static library target; the build action
        # will build both static and dynamic as a result):
        return env._BazelBuild(target=f"{scons_outfile_base}.a",
                               source=[])  # `source` is required, even though it is empty

    if env.get("BAZEL_BUILD_ENABLED"):
        # Bail if current architecture not supported for Bazel:
        current_architecture = platform.machine()
        supported_architectures = ['aarch64']
        if current_architecture not in supported_architectures:
            raise Exception(
                f'Bazel not supported on this architecture ({current_architecture}); supported architectures are: [{supported_architectures}]'
            )
        # TODO(SERVER-81038): remove once bazel/bazelisk is added to the toolchain.
        if not os.path.exists("bazelisk"):
            urllib.request.urlretrieve(
                "https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-linux-arm64",
                "bazelisk")
            os.chmod("bazelisk", stat.S_IXUSR)

        BAZELISK_PATH = os.path.abspath("bazelisk")
        env['BUILDERS']['BazelLibrary'] = bazel_library
    else:
        env['BUILDERS']['BazelLibrary'] = env['BUILDERS']['Library']
