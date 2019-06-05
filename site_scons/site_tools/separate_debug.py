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


def _update_builder(env, builder, bitcode):

    old_scanner = builder.target_scanner
    old_path_function = old_scanner.path_function

    def new_scanner(node, env, path=()):
        results = old_scanner.function(node, env, path)
        origin = getattr(node.attributes, "debug_file_for", None)
        if origin:
            origin_results = old_scanner(origin, env, path)
            for origin_result in origin_results:
                origin_result_debug_file = getattr(origin_result.attributes, "separate_debug_file",
                                                   None)
                if origin_result_debug_file:
                    results.append(origin_result_debug_file)
        # TODO: Do we need to do the same sort of drag along for bcsymbolmap files?
        return results

    builder.target_scanner = SCons.Scanner.Scanner(
        function=new_scanner,
        path_function=old_path_function,
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
    if env.TargetOSIs('darwin'):
        if bitcode:
            base_action.list.append(
                SCons.Action.Action(
                    "dsymutil -num-threads=1 $TARGET --symbol-map=${TARGET}.bcsymbolmap -o ${TARGET}.dSYM",
                    "Generating debug info for $TARGET into ${TARGET}.dSYM"))

        else:
            base_action.list.append(
                SCons.Action.Action("dsymutil -num-threads=1 $TARGET -o ${TARGET}.dSYM",
                                    "Generating debug info for $TARGET into ${TARGET}.dSYM"))
        base_action.list.append(SCons.Action.Action("strip -Sx ${TARGET}", "Stripping ${TARGET}"))
    elif env.TargetOSIs('posix'):
        base_action.list.extend([
            SCons.Action.Action("${OBJCOPY} --only-keep-debug $TARGET ${TARGET}.debug",
                                "Generating debug info for $TARGET into ${TARGET}.debug"),
            SCons.Action.Action(
                "${OBJCOPY} --strip-debug --add-gnu-debuglink ${TARGET}.debug ${TARGET}",
                "Stripping debug info from ${TARGET} and adding .gnu.debuglink to ${TARGET}.debug"),
        ])
    else:
        pass

    base_emitter = builder.emitter

    def new_emitter(target, source, env):

        bitcode_file = None
        if env.TargetOSIs('darwin'):
            debug_file = env.Dir(str(target[0]) + ".dSYM")
            if bitcode:
                bitcode_file = env.File(str(target[0]) + ".bcsymbolmap")
        elif env.TargetOSIs('posix'):
            debug_file = env.File(str(target[0]) + ".debug")
        else:
            pass

        setattr(debug_file.attributes, "debug_file_for", target[0])
        setattr(target[0].attributes, "separate_debug_file", debug_file)

        target.append(debug_file)

        if bitcode_file:
            setattr(bitcode_file.attributes, "bcsymbolmap_file_for", target[0])
            setattr(target[0].attributes, "bcsymbolmap_file", bitcode_file)
            target.append(bitcode_file)

        return (target, source)

    new_emitter = SCons.Builder.ListEmitter([base_emitter, new_emitter])
    builder.emitter = new_emitter


def generate(env):
    if not exists(env):
        return

    # If we are generating bitcode, add the magic linker flags that
    # hide the bitcode symbols, and override the name of the bitcode
    # symbol map file so that it is determinstically known to SCons
    # rather than being a UUID. We need this so that we can install it
    # under a well known name. We leave it to the evergreen
    # postprocessing to rename to the correct name. I'd like to do
    # this better, but struggled for a long time and decided that
    # later was a better time to address this. We should also consider
    # moving all bitcode setup into a separate tool.
    bitcode = False
    if env.TargetOSIs('darwin') and any(flag == "-fembed-bitcode" for flag in env['LINKFLAGS']):
        bitcode = True
        env.AppendUnique(LINKFLAGS=[
            "-Wl,-bitcode_hide_symbols",
            "-Wl,-bitcode_symbol_map,${TARGET}.bcsymbolmap",
        ])

    # TODO: For now, not doing this for programs. Need to update
    # auto_install_binaries to understand to install the debug symbol
    # for target X to the same target location as X.
    for builder in ['SharedLibrary', 'LoadableModule']:
        _update_builder(env, env['BUILDERS'][builder], bitcode)


def exists(env):
    return True
