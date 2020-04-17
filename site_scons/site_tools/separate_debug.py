# Copyright 2018 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import SCons


def _update_builder(env, builder):

    old_scanner = builder.target_scanner
    old_path_function = old_scanner.path_function

    def new_scanner(node, env, path=()):
        results = old_scanner.function(node, env, path)
        origin = getattr(node.attributes, "debug_file_for", None)
        if origin is not None:
            origin_results = old_scanner(origin, env, path)
            for origin_result in origin_results:
                origin_result_debug_files = getattr(
                    origin_result.attributes, "separate_debug_files", None
                )
                if origin_result_debug_files is not None:
                    results.extend(origin_result_debug_files)
        return results

    builder.target_scanner = SCons.Scanner.Scanner(
        function=new_scanner, path_function=old_path_function,
    )

    base_action = builder.action
    if not isinstance(base_action, SCons.Action.ListAction):
        base_action = SCons.Action.ListAction([base_action])

    # TODO: Make variables for dsymutil and strip, and for the action
    # strings. We should really be running these tools as found by
    # xcrun by default. We should achieve that by upgrading the
    # site_scons/site_tools/xcode.py tool to search for these for
    # us. We could then also remove a lot of the compiler and sysroot
    # setup from the etc/scons/xcode_*.vars files, which would be a
    # win as well.
    if env.TargetOSIs("darwin"):
        base_action.list.extend(
            [
                SCons.Action.Action(
                    "$DSYMUTIL -num-threads 1 $TARGET -o ${TARGET}.dSYM",
                    "$DSYMUTILCOMSTR"
                ),
                SCons.Action.Action(
                    "$STRIP -Sx ${TARGET}",
                    "$DEBUGSTRIPCOMSTR"
                ),
            ]
        )
    elif env.TargetOSIs("posix"):
        base_action.list.extend(
            [
                SCons.Action.Action(
                    "$OBJCOPY --only-keep-debug $TARGET ${TARGET}.debug",
                    "$OBJCOPY_ONLY_KEEP_DEBUG_COMSTR"
                ),
                SCons.Action.Action(
                    "$OBJCOPY --strip-debug --add-gnu-debuglink ${TARGET}.debug ${TARGET}",
                    "$DEBUGSTRIPCOMSTR"
                ),
            ]
        )
    else:
        pass

    builder.action = base_action

    base_emitter = builder.emitter

    def new_emitter(target, source, env):

        debug_files = []
        if env.TargetOSIs("darwin"):

            # There isn't a lot of great documentation about the structure of dSYM bundles.
            # For general bundles, see:
            #
            # https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFBundles/BundleTypes/BundleTypes.html
            #
            # But we expect to find two files in the bundle. An
            # Info.plist file under Contents, and a file with the same
            # name as the target under Contents/Resources/DWARF.

            target0 = env.File(target[0])
            dsym_dir_name = str(target[0]) + ".dSYM"
            dsym_dir = env.Dir(dsym_dir_name, directory=target0.get_dir())

            plist_file = env.File("Contents/Info.plist", directory=dsym_dir)
            setattr(plist_file.attributes, "aib_effective_suffix", ".dSYM")
            setattr(plist_file.attributes, "aib_additional_directory", "{}/Contents".format(dsym_dir_name))

            dwarf_dir = env.Dir("Contents/Resources/DWARF", directory=dsym_dir)

            dwarf_file = env.File(target0.name, directory=dwarf_dir)
            setattr(dwarf_file.attributes, "aib_effective_suffix", ".dSYM")
            setattr(dwarf_file.attributes, "aib_additional_directory", "{}/Contents/Resources/DWARF".format(dsym_dir_name))

            debug_files.extend([plist_file, dwarf_file])

        elif env.TargetOSIs("posix"):
            debug_file = env.File(str(target[0]) + ".debug")
            debug_files.append(debug_file)
        elif env.TargetOSIs("windows"):
            debug_file = env.File(env.subst("${PDB}", target=target))
            debug_files.append(debug_file)
        else:
            pass

        # Establish bidirectional linkages between the target and each debug file by setting
        # attributes on th nodes. We use these in the scanner above to ensure that transitive
        # dependencies among libraries are projected into transitive dependencies between
        # debug files.
        for debug_file in debug_files:
            setattr(debug_file.attributes, "debug_file_for", target[0])
        setattr(target[0].attributes, "separate_debug_files", debug_files)

        return (target + debug_files, source)

    new_emitter = SCons.Builder.ListEmitter([base_emitter, new_emitter])
    builder.emitter = new_emitter


def generate(env):
    if not exists(env):
        return

    if env.TargetOSIs("darwin"):

        if env.get("DSYMUTIL", None) is None:
            env["DSYMUTIL"] = env.WhereIs("dsymutil")

        if env.get("STRIP", None) is None:
            env["STRIP"] = env.WhereIs("strip")

        if not env.Verbose():
            env.Append(
                DSYMUTILCOMSTR="Generating debug info for $TARGET into ${TARGET}.dSYM",
                DEBUGSTRIPCOMSTR="Stripping debug info from ${TARGET}",
            )

    elif env.TargetOSIs("posix"):
        if env.get("OBJCOPY", None) is None:
            env["OBJCOPY"] = env.Whereis("objcopy")

        if not env.Verbose():
            env.Append(
                OBJCOPY_ONLY_KEEP_DEBUG_COMSTR="Generating debug info for $TARGET into ${TARGET}.dSYM",
                DEBUGSTRIPCOMSTR="Stripping debug info from ${TARGET} and adding .gnu.debuglink to ${TARGET}.debug",
            )

    for builder in ["Program", "SharedLibrary", "LoadableModule"]:
        _update_builder(env, env["BUILDERS"][builder])


def exists(env):
    if env.TargetOSIs("darwin"):
        if env.get("DSYMUTIL", None) is None and env.WhereIs("dsymutil") is None:
            return False
        if env.get("STRIP", None) is None and env.WhereIs("strip") is None:
            return False
    elif env.TargetOSIs("posix"):
        if env.get("OBJCOPY", None) is None and env.WhereIs("objcopy") is None:
            return False
    return True
