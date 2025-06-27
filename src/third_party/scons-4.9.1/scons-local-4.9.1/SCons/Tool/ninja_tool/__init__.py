# MIT License
#
# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

"""Generate build.ninja files from SCons aliases."""

import importlib
import os
import traceback
import subprocess
import sys

import SCons
import SCons.Script
from SCons.Tool.ninja_tool.Globals import ninja_builder_initialized
from SCons.Script import GetOption
from SCons.Util import sanitize_shell_env

from .Globals import NINJA_RULES, NINJA_POOLS, NINJA_CUSTOM_HANDLERS, NINJA_DEFAULT_TARGETS, NINJA_CMDLINE_TARGETS
from .Methods import register_custom_handler, register_custom_rule_mapping, register_custom_rule, register_custom_pool, \
    set_build_node_callback, get_generic_shell_command, CheckNinjaCompdbExpand, get_command, \
    gen_get_response_file_command
from .Overrides import ninja_hack_linkcom, ninja_hack_arcom, NinjaNoResponseFiles, ninja_always_serial, AlwaysExecAction
from .Utils import ninja_add_command_line_options, \
    ninja_noop, ninja_print_conf_log, ninja_csig, ninja_contents, ninja_stat, ninja_whereis, NinjaExperimentalWarning

try:
    import ninja
    NINJA_BINARY = ninja.__file__
except ImportError:
    NINJA_BINARY = False
else:
    from .NinjaState import NinjaState

NINJA_STATE = None


def ninja_builder(env, target, source):
    """Generate a build.ninja for source."""
    if not isinstance(source, list):
        source = [source]
    if not isinstance(target, list):
        target = [target]

    # We have no COMSTR equivalent so print that we're generating
    # here.
    print("Generating:", str(target[0]))

    generated_build_ninja = target[0].get_abspath()
    try:
        NINJA_STATE.generate()
    except Exception:
        raise SCons.Errors.BuildError(
            errstr=f"ERROR: an exception occurred while generating the ninja file:\n{traceback.format_exc()}",
            node=target)

    if env["PLATFORM"] == "win32":
        # TODO: Is this necessary as you set env variable in the ninja build file per target?
        # this is not great, its doesn't consider specific
        # node environments, which means on linux the build could
        # behave differently, because on linux you can set the environment
        # per command in the ninja file. This is only needed if
        # running ninja directly from a command line that hasn't
        # had the environment setup (vcvarsall.bat)
        with open('run_ninja_env.bat', 'w') as f:
            for key in env['ENV']:
                f.write('set {}={}\n'.format(key, env['ENV'][key]))
            f.write(f'{NINJA_STATE.ninja_bin_path} -f {generated_build_ninja} %*\n')
        cmd = ['run_ninja_env.bat']

    else:
        cmd = [NINJA_STATE.ninja_bin_path, '-f', generated_build_ninja]

    if str(env.get("NINJA_DISABLE_AUTO_RUN")).lower() not in ['1', 'true']:
        num_jobs = env.get('NINJA_MAX_JOBS', env.GetOption("num_jobs"))
        cmd += ['-j' + str(num_jobs)] + env.get('NINJA_CMD_ARGS', '').split() + NINJA_CMDLINE_TARGETS
        print(f"ninja will be run with command line targets: {' '.join(NINJA_CMDLINE_TARGETS)}")
        print("Executing:", str(' '.join(cmd)))

        # execute the ninja build at the end of SCons, trying to
        # reproduce the output like a ninja build would
        def execute_ninja():

            if env['PLATFORM'] == 'win32':
                spawn_env = os.environ
            else:
                spawn_env = sanitize_shell_env(env['ENV'])

            proc = subprocess.Popen(cmd,
                                    stderr=sys.stderr,
                                    stdout=subprocess.PIPE,
                                    universal_newlines=True,
                                    env=spawn_env
                                    )
            yield from iter(proc.stdout.readline, "")
            proc.stdout.close()
            return_code = proc.wait()
            if return_code:
                raise subprocess.CalledProcessError(return_code, 'ninja')

        erase_previous = False
        for output in execute_ninja():
            output = output.strip()
            if erase_previous:
                sys.stdout.write('\x1b[2K')  # erase previous line
                sys.stdout.write("\r")
            else:
                sys.stdout.write(os.linesep)
            sys.stdout.write(output)
            sys.stdout.flush()
            # this will only erase ninjas [#/#] lines
            # leaving warnings and other output, seems a bit
            # prone to failure with such a simple check
            erase_previous = output.startswith('[')
        sys.stdout.write("\n")


def options(opts) -> None:
    """
    Add command line Variables for Ninja builder.
    """
    opts.AddVariables(
        ("NINJA_CMD_ARGS", "Arguments to pass to ninja"),
    )

def exists(env):
    """Enable if called."""

    if 'ninja' not in GetOption('experimental'):
        return False

    # This variable disables the tool when storing the SCons command in the
    # generated ninja file to ensure that the ninja tool is not loaded when
    # SCons should do actual work as a subprocess of a ninja build. The ninja
    # tool is very invasive into the internals of SCons and so should never be
    # enabled when SCons needs to build a target.
    if env.get("__NINJA_NO", "0") == "1":
        return False

    # pypi ninja module detection done at top of file during import ninja.
    if NINJA_BINARY:
        return NINJA_BINARY
    else:
        raise SCons.Warnings.SConsWarning("Failed to import ninja, attempt normal SCons build.")


def ninja_emitter(target, source, env):
    """ fix up the source/targets """

    ninja_file = env.File(env.subst("$NINJA_FILE_NAME"))
    ninja_file.attributes.ninja_file = True

    # Someone called env.Ninja('my_targetname.ninja')
    if not target and len(source) == 1:
        target = source

    # Default target name is $NINJA_PREFIX.$NINJA.SUFFIX
    if not target:
        target = [ninja_file, ]

    # No source should have been passed. Drop it.
    if source:
        source = []

    return target, source


def generate(env):
    """Generate the NINJA builders."""

    global NINJA_STATE, NINJA_CMDLINE_TARGETS, ninja_builder_initialized

    if 'ninja' not in GetOption('experimental'):
        return

    if not ninja_builder_initialized:
        ninja_builder_initialized = True

        ninja_add_command_line_options()

    if not NINJA_BINARY:
        raise SCons.Warnings.SConsWarning("Failed to import ninja, attempt normal SCons build.")

    env["NINJA_DISABLE_AUTO_RUN"] = env.get("NINJA_DISABLE_AUTO_RUN", GetOption('disable_execute_ninja'))
    env["NINJA_FILE_NAME"] = env.get("NINJA_FILE_NAME", "build.ninja")

    # Add the Ninja builder.
    always_exec_ninja_action = AlwaysExecAction(ninja_builder, {})
    ninja_builder_obj = SCons.Builder.Builder(action=always_exec_ninja_action,
                                              emitter=ninja_emitter)
    env.Append(BUILDERS={"Ninja": ninja_builder_obj})

    env["NINJA_ALIAS_NAME"] = env.get("NINJA_ALIAS_NAME", "generate-ninja")
    env['NINJA_DIR'] = env.Dir(env.get("NINJA_DIR", '#/.ninja'))
    env["NINJA_SCONS_DAEMON_KEEP_ALIVE"] = env.get("NINJA_SCONS_DAEMON_KEEP_ALIVE", 180000)

    if GetOption("disable_ninja"):
        env.SConsignFile(os.path.join(str(env['NINJA_DIR']), '.ninja.sconsign'))

    # here we allow multiple environments to construct rules and builds
    # into the same ninja file
    if NINJA_STATE is None:
        ninja_file = env.Ninja()
        env['NINJA_FILE'] = ninja_file[0]
        env.AlwaysBuild(ninja_file)

        # We need to force SCons to only build the ninja target when ninja tool is loaded.
        # The ninja tool is going to 'rip the guts out' of scons and make it basically unable
        # to do anything in terms of building, so any targets besides the ninja target will
        # end up doing nothing besides causing confusion. We save the targets however, so that
        # SCons and invoke ninja to build them in lieu of the user.
        NINJA_CMDLINE_TARGETS = SCons.Script.BUILD_TARGETS
        SCons.Script.BUILD_TARGETS = SCons.Script.TargetList(env.Alias("$NINJA_ALIAS_NAME", ninja_file))
    else:
        if str(NINJA_STATE.ninja_file) != env["NINJA_FILE_NAME"]:
            SCons.Warnings.SConsWarning("Generating multiple ninja files not supported, set ninja file name before tool initialization.")
        ninja_file = [NINJA_STATE.ninja_file]


    def ninja_generate_deps(env):
        """Return a list of SConscripts
        TODO: Should we also include files loaded from site_scons/***
          or even all loaded modules? https://stackoverflow.com/questions/4858100/how-to-list-imported-modules
        TODO: Do we want this to be Nodes?
        """
        return sorted([str(s) for s in SCons.Node.SConscriptNodes])

    env['_NINJA_REGENERATE_DEPS_FUNC'] = ninja_generate_deps

    env['NINJA_REGENERATE_DEPS'] = env.get('NINJA_REGENERATE_DEPS', '${_NINJA_REGENERATE_DEPS_FUNC(__env__)}')

    # This adds the required flags such that the generated compile
    # commands will create depfiles as appropriate in the Ninja file.
    if 'CCDEPFLAGS' not in env:
        # Issue some warning here
        pass
    else:
        env.Append(CCFLAGS='$CCDEPFLAGS')

    env.AddMethod(CheckNinjaCompdbExpand, "CheckNinjaCompdbExpand")

    # Provide a way for custom rule authors to easily access command
    # generation.
    env.AddMethod(get_generic_shell_command, "NinjaGetGenericShellCommand")
    env.AddMethod(get_command, "NinjaGetCommand")
    env.AddMethod(gen_get_response_file_command, "NinjaGenResponseFileProvider")
    env.AddMethod(set_build_node_callback, "NinjaSetBuildNodeCallback")

    # Provides a way for users to handle custom FunctionActions they
    # want to translate to Ninja.
    env[NINJA_CUSTOM_HANDLERS] = {}
    env.AddMethod(register_custom_handler, "NinjaRegisterFunctionHandler")

    # Provides a mechanism for inject custom Ninja rules which can
    # then be mapped using NinjaRuleMapping.
    env[NINJA_RULES] = {}
    env.AddMethod(register_custom_rule, "NinjaRule")

    # Provides a mechanism for inject custom Ninja pools which can
    # be used by providing the NINJA_POOL="name" as an
    # OverrideEnvironment variable in a builder call.
    env[NINJA_POOLS] = {}
    env.AddMethod(register_custom_pool, "NinjaPool")

    # Add the ability to register custom NinjaRuleMappings for Command
    # builders. We don't store this dictionary in the env to prevent
    # accidental deletion of the CC/XXCOM mappings. You can still
    # overwrite them if you really want to but you have to explicit
    # about it this way. The reason is that if they were accidentally
    # deleted you would get a very subtly incorrect Ninja file and
    # might not catch it.
    env.AddMethod(register_custom_rule_mapping, "NinjaRuleMapping")

    # on windows we need to change the link action
    ninja_hack_linkcom(env)

    # Normally in SCons actions for the Program and *Library builders
    # will return "${*COM}" as their pre-subst'd command line. However
    # if a user in a SConscript overwrites those values via key access
    # like env["LINKCOM"] = "$( $ICERUN $)" + env["LINKCOM"] then
    # those actions no longer return the "bracketted" string and
    # instead return something that looks more expanded. So to
    # continue working even if a user has done this we map both the
    # "bracketted" and semi-expanded versions.
    def robust_rule_mapping(var, rule, tool) -> None:
        provider = gen_get_response_file_command(env, rule, tool)
        env.NinjaRuleMapping("${" + var + "}", provider)

        # some of these construction vars could be generators, e.g.
        # CommandGeneratorAction, so if the var is not a string, we
        # can't parse the generated string.
        if isinstance(env.get(var), str):
            env.NinjaRuleMapping(env.get(var, None), provider)

    robust_rule_mapping("CCCOM", "CC", "$CC")
    robust_rule_mapping("SHCCCOM", "CC", "$CC")
    robust_rule_mapping("CXXCOM", "CXX", "$CXX")
    robust_rule_mapping("SHCXXCOM", "CXX", "$CXX")
    robust_rule_mapping("LINKCOM", "LINK", "$LINK")
    robust_rule_mapping("SHLINKCOM", "LINK", "$SHLINK")
    robust_rule_mapping("ARCOM", "AR", "$AR")

    # Make SCons node walk faster by preventing unnecessary work
    env.Decider("timestamp-match")

    # Used to determine if a build generates a source file. Ninja
    # requires that all generated sources are added as order_only
    # dependencies to any builds that *might* use them.
    # TODO: switch to using SCons to help determine this (Github Issue #3624)
    env["NINJA_GENERATED_SOURCE_SUFFIXES"] = env.get('NINJA_GENERATED_SOURCE_SUFFIXES', [".h", ".hpp"])

    # Force ARCOM so use 's' flag on ar instead of separately running ranlib
    ninja_hack_arcom(env)

    if GetOption('disable_ninja'):
        return env

    print("Initializing ninja tool... this feature is experimental. SCons internals and all environments will be affected.")
    print(f"SCons running in ninja mode. {env['NINJA_FILE']} will be generated.")
    # This is the point of no return, anything after this comment
    # makes changes to SCons that are irreversible and incompatible
    # with a normal SCons build. We return early if __NINJA_NO=1 has
    # been given on the command line (i.e. by us in the generated
    # ninja file) here to prevent these modifications from happening
    # when we want SCons to do work. Everything before this was
    # necessary to setup the builder and other functions so that the
    # tool can be unconditionally used in the users's SCons files.

    if not exists(env):
        return

    # Set a known variable that other tools can query so they can
    # behave correctly during ninja generation.
    env["GENERATING_NINJA"] = True

    # These methods are no-op'd because they do not work during ninja
    # generation, expected to do no work, or simply fail. All of which
    # are slow in SCons. So we overwrite them with no logic.
    SCons.Node.FS.File.make_ready = ninja_noop
    SCons.Node.FS.File.prepare = ninja_noop
    SCons.Node.FS.File.push_to_cache = ninja_noop
    SCons.Executor.Executor.prepare = ninja_noop
    SCons.Taskmaster.Task.prepare = ninja_noop
    SCons.Node.FS.File.built = ninja_noop
    SCons.Node.Node.visited = ninja_noop

    # We make lstat a no-op because it is only used for SONAME
    # symlinks which we're not producing.
    SCons.Node.FS.LocalFS.lstat = ninja_noop

    # This is a slow method that isn't memoized. We make it a noop
    # since during our generation we will never use the results of
    # this or change the results.
    SCons.Node.FS.is_up_to_date = ninja_noop

    # We overwrite stat and WhereIs with eternally memoized
    # implementations. See the docstring of ninja_stat and
    # ninja_whereis for detailed explanations.
    SCons.Node.FS.LocalFS.stat = ninja_stat
    SCons.Util.WhereIs = ninja_whereis

    # Monkey patch get_csig and get_contents for some classes. It
    # slows down the build significantly and we don't need contents or
    # content signatures calculated when generating a ninja file since
    # we're not doing any SCons caching or building.
    SCons.Executor.Executor.get_contents = ninja_contents(
        SCons.Executor.Executor.get_contents
    )
    SCons.Node.Alias.Alias.get_contents = ninja_contents(
        SCons.Node.Alias.Alias.get_contents
    )
    SCons.Node.FS.File.get_contents = ninja_contents(SCons.Node.FS.File.get_contents)
    SCons.Node.FS.File.get_csig = ninja_csig(SCons.Node.FS.File.get_csig)
    SCons.Node.FS.Dir.get_csig = ninja_csig(SCons.Node.FS.Dir.get_csig)
    SCons.Node.Alias.Alias.get_csig = ninja_csig(SCons.Node.Alias.Alias.get_csig)

    # Ignore CHANGED_SOURCES and CHANGED_TARGETS. We don't want those
    # to have effect in a generation pass because the generator
    # shouldn't generate differently depending on the current local
    # state. Without this, when generating on Windows, if you already
    # had a foo.obj, you would omit foo.cpp from the response file. Do the same for UNCHANGED.
    SCons.Executor.Executor._get_changed_sources = SCons.Executor.Executor._get_sources
    SCons.Executor.Executor._get_changed_targets = SCons.Executor.Executor._get_targets
    SCons.Executor.Executor._get_unchanged_sources = SCons.Executor.Executor._get_sources
    SCons.Executor.Executor._get_unchanged_targets = SCons.Executor.Executor._get_targets

    # Replace false action messages with nothing.
    env["PRINT_CMD_LINE_FUNC"] = ninja_print_conf_log

    # This reduces unnecessary subst_list calls to add the compiler to
    # the implicit dependencies of targets. Since we encode full paths
    # in our generated commands we do not need these slow subst calls
    # as executing the command will fail if the file is not found
    # where we expect it.
    env["IMPLICIT_COMMAND_DEPENDENCIES"] = False

    # This makes SCons more aggressively cache MD5 signatures in the
    # SConsign file.
    # TODO: WPD shouldn't this be set to 0?
    env.SetOption("max_drift", 1)

    # The Serial job class is SIGNIFICANTLY (almost twice as) faster
    # than the Parallel job class for generating Ninja files. So we
    # monkey the Jobs constructor to only use the Serial Job class.
    SCons.Taskmaster.Job.Jobs.__init__ = ninja_always_serial

    ninja_syntax = importlib.import_module(".ninja_syntax", package='ninja')

    if NINJA_STATE is None:
        NINJA_STATE = NinjaState(env, ninja_file[0], ninja_syntax)

    # TODO: this is hacking into scons, preferable if there were a less intrusive way
    # We will subvert the normal builder execute to make sure all the ninja file is dependent
    # on all targets generated from any builders
    SCons_Builder_BuilderBase__execute = SCons.Builder.BuilderBase._execute

    def NinjaBuilderExecute(self, env, target, source, overwarn={}, executor_kw={}):
        # this ensures all environments in which a builder executes from will
        # not create list actions for linking on windows
        ninja_hack_linkcom(env)
        targets = SCons_Builder_BuilderBase__execute(self, env, target, source, overwarn=overwarn, executor_kw=executor_kw)

        if not SCons.Util.is_List(target):
            target = [target]

        for target in targets:
            if target.check_attributes('ninja_file') is None and not target.is_conftest():
                env.Depends(ninja_file, targets)
        return targets
    SCons.Builder.BuilderBase._execute = NinjaBuilderExecute

    # Here we monkey patch the Task.execute method to not do a bunch of
    # unnecessary work. If a build is a regular builder (i.e not a conftest and
    # not our own Ninja builder) then we add it to the NINJA_STATE. Otherwise we
    # build it like normal. This skips all of the caching work that this method
    # would normally do since we aren't pulling any of these targets from the
    # cache.
    #
    # In the future we may be able to use this to actually cache the build.ninja
    # file once we have the upstream support for referencing SConscripts as File
    # nodes.
    def ninja_execute(self) -> None:

        target = self.targets[0]
        if target.get_env().get('NINJA_SKIP'):
            return
        if target.check_attributes('ninja_file') is None:
            NINJA_STATE.add_build(target)
        else:
            target.build()

    SCons.Taskmaster.Task.execute = ninja_execute

    # Make needs_execute always return true instead of determining out of
    # date-ness.
    SCons.Script.Main.BuildTask.needs_execute = lambda x: True

    def ninja_Set_Default_Targets(env, tlist) -> None:
        """
            Record the default targets if they were ever set by the user. Ninja
            will need to write the default targets and make sure not to include
            the scons daemon shutdown target.
        """
        SCons.Script._Get_Default_Targets = SCons.Script._Set_Default_Targets_Has_Been_Called
        SCons.Script.DEFAULT_TARGETS = ninja_file
        for t in tlist:
            if isinstance(t, SCons.Node.Node):
                NINJA_DEFAULT_TARGETS.append(t)
            else:
                nodes = env.arg2nodes(t, env.fs.Entry)
                NINJA_DEFAULT_TARGETS.extend(nodes)
    SCons.Script._Set_Default_Targets = ninja_Set_Default_Targets

    # We will eventually need to overwrite TempFileMunge to make it
    # handle persistent tempfiles or get an upstreamed change to add
    # some configurability to it's behavior in regards to tempfiles.
    #
    # Set all three environment variables that Python's
    # tempfile.mkstemp looks at as it behaves differently on different
    # platforms and versions of Python.
    # build_dir = env.subst("$NINJA_DIR")
    # if build_dir == "":
    #     build_dir = "."
    # os.environ["TMPDIR"] = env.Dir("{}/.response_files".format(build_dir)).get_abspath()
    # os.environ["TEMP"] = os.environ["TMPDIR"]
    # os.environ["TMP"] = os.environ["TMPDIR"]
    # if not os.path.isdir(os.environ["TMPDIR"]):
    #     env.Execute(SCons.Defaults.Mkdir(os.environ["TMPDIR"]))

    env['TEMPFILEDIR'] = "$NINJA_DIR/.response_files"
    env["TEMPFILE"] = NinjaNoResponseFiles

    env.Alias('run-ninja-scons-daemon', 'run_ninja_scons_daemon_phony')
    env.Alias('shutdown-ninja-scons-daemon', 'shutdown_ninja_scons_daemon_phony')
