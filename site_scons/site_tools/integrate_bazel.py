import os
import SCons

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
        #   target_to_build: fsync_locked
        #   cwd:             /home/ubuntu/mongo/build/fast/mongo/db/commands
        #   bazel_dir:       src/mongo/db/command
        #   bazel_target:    //src/mongo/db/commands:fsync_locked
        #   bazel_outfile:   bazel-bin/src/mongo/db/commands/libfsync_locked.a
        #   scons_outfile:   /home/ubuntu/mongo/build/fast/mongo/db/commands/libfsync_locked.a
        target_to_build = target[0]
        cwd = os.getcwd()
        bazel_dir = os.path.dirname(env.File(os.path.join(cwd, target_to_build)).srcnode().path)
        bazel_target = f"//{bazel_dir}:{target_to_build}"
        bazel_outfile = f"bazel-bin/{bazel_dir}/lib{target_to_build}.a"
        scons_outfile = f"{cwd}/lib{target_to_build}.a"

        # Craft an action that builds the target and colocates it with Scons output, and invoke the action:
        action = f"{BAZELISK_PATH} build {bazel_target} && cp -f {bazel_outfile} {scons_outfile}"
        env['BUILDERS']['_BazelBuild'] = SCons.Builder.Builder(action=action)
        return env._BazelBuild(target=scons_outfile,
                               source=[])  # `source` is required, even though it is empty

    if env.get("BAZEL_BUILD_ENABLED"):
        if not os.path.isfile(BAZELISK_PATH):
            raise Exception(
                f"Bazelisk installation is missing; please see docs/bazel.md for setup instructions"
            )
        env['BUILDERS']['BazelLibrary'] = bazel_library
    else:
        env['BUILDERS']['BazelLibrary'] = env['BUILDERS']['Library']
