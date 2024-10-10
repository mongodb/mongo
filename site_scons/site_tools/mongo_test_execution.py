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

import os

import auto_install_binaries
import SCons
from SCons.Node.Alias import default_ans

_proof_scanner_cache_key = "proof_scanner_cache"
_associated_proof = "associated_proof_key"


def proof_generator_command_scanner_func(node, env, path):
    results = getattr(node.attributes, _proof_scanner_cache_key, None)
    if results is not None:
        return results
    results = env.GetTransitivelyInstalledFiles(node)
    setattr(node.attributes, _proof_scanner_cache_key, results)
    return results


proof_generator_command_scanner = SCons.Scanner.Scanner(
    function=proof_generator_command_scanner_func,
    path_function=None,
    recursive=True,
)


def auto_prove_task(env, component, role):
    entry = auto_install_binaries.get_alias_map_entry(env, component, role)
    return [
        getattr(f.attributes, _associated_proof)
        for f in entry.files
        if hasattr(f.attributes, _associated_proof)
    ]


def generate_test_execution_aliases(env, test):
    installed = [test]
    if env.get("AUTO_INSTALL_ENABLED", False) and env.GetAutoInstalledFiles(test):
        installed = env.GetAutoInstalledFiles(test)

    target_name = os.path.basename(installed[0].path)

    test_env = env.Clone()
    test_env["ENV"]["TMPDIR"] = test_env.Dir("$LOCAL_TMPDIR").abspath
    target_command = test_env.Command(
        target=f"#+{target_name}",
        source=installed[0],
        action="$( $ICERUN $) ${SOURCES[0]} $UNITTEST_FLAGS",
        NINJA_POOL="console",
    )
    env.Pseudo(target_command)
    env.Alias("test-execution-aliases", target_command)

    for source in test.sources:
        source_base_name = os.path.basename(source.get_path())
        # Strip suffix
        dot_idx = source_base_name.rfind(".")
        suffix = source_base_name[dot_idx:]
        if suffix in env["TEST_EXECUTION_SUFFIX_DENYLIST"]:
            continue

        source_name = source_base_name[:dot_idx]

        # We currently create two types of commands: legacy and verbose
        # ex legacy command: cancelable_operation_context_test
        # ex verbose command: db_unittest_test_cancelable_operation_context_test
        # i.e. Verbose incorporates the name of the unittest binary, while
        # legacy only has the source file name.
        # We always create the verbose command, but we only create the legacy
        # command if there isn't a conflict between the target_name and
        # source_name. Legacy commands must be unique
        verbose_source_command = test_env.Command(
            target=f"#+{target_name}-{source_name}",
            source=installed[0],
            action="$( $ICERUN $) ${SOURCES[0]} -fileNameFilter $TEST_SOURCE_FILE_NAME $UNITTEST_FLAGS",
            TEST_SOURCE_FILE_NAME=source_name,
            NINJA_POOL="console",
        )
        env.Pseudo(verbose_source_command)
        env.Alias("test-execution-aliases", verbose_source_command)

        if target_name == source_name:
            continue

        if default_ans.lookup(f"+{source_name}") is not None:
            raise SCons.Errors.BuildError(
                str(verbose_source_command[0]),
                f"There exists multiple unittests with a source file named {source_name}: {source.abspath} and {env.Alias(f'+{source_name}')[0].children()[1].abspath}",
            )
        env.Alias(f"+{source_name}", [verbose_source_command, source])

    proof_generator_command = test_env.Command(
        target=[
            "${SOURCE}.log",
            "${SOURCE}.status",
        ],
        source=installed[0],
        action=SCons.Action.Action("$PROOF_GENERATOR_COMMAND", "$PROOF_GENERATOR_COMSTR"),
        source_scanner=proof_generator_command_scanner,
    )

    # We assume tests are provable by default, but some tests may not
    # be. Such tests can be tagged with UNDECIDABLE_TEST=True. If a
    # test isn't provable, we disable caching its results and require
    # it to be always rebuilt.
    if installed[0].env.get("UNDECIDABLE_TEST", False):
        env.NoCache(proof_generator_command)
        env.AlwaysBuild(proof_generator_command)

    proof_analyzer_command = test_env.Command(
        target="${SOURCES[1].base}.proof",
        source=proof_generator_command,
        action=SCons.Action.Action("$PROOF_ANALYZER_COMMAND", "$PROOF_ANALYZER_COMSTR"),
    )

    proof_analyzer_alias = env.Alias(
        f"prove-{target_name}",
        proof_analyzer_command,
    )

    setattr(installed[0].attributes, _associated_proof, proof_analyzer_alias)

    # TODO: Should we enable proof at the file level?


def exists(env):
    return True


def generate(env):
    # Used for Ninja generator to collect the test execution aliases
    env.Alias("test-execution-aliases")
    env.AddMethod(generate_test_execution_aliases, "GenerateTestExecutionAliases")

    env["TEST_EXECUTION_SUFFIX_DENYLIST"] = env.get(
        "TEST_EXECUTION_SUFFIX_DENYLIST",
        [".in"],
    )

    env.AppendUnique(
        AIB_TASKS={
            "prove": (auto_prove_task, False),
        }
    )

    # TODO: Should we have some sort of prefix_xdir for the output location for these? Something like
    # $PREFIX_VARCACHE and which in our build is pre-populated to $PREFIX/var/cache/mongo or similar?

    if env["PLATFORM"] == "win32":
        env["PROOF_GENERATOR_COMMAND"] = (
            "$( $ICERUN $) ${SOURCES[0]} $UNITTEST_FLAGS > ${TARGETS[0]} 2>&1 & call echo %^errorlevel% > ${TARGETS[1]}"
        )

        # Keeping this here for later, but it only works if cmd.exe is
        # launched with /V, and SCons doesn't do that.
        #
        # env["PROOF_ANALYZER_COMMAND"] = "set /p nextErrorLevel=<${SOURCES[1]} & if \"!nextErrorLevel!\"==\"0 \" (type nul > $TARGET) else (exit 1)"
        #
        # Instead, use grep! I mean findstr.
        env["PROOF_ANALYZER_COMMAND"] = (
            "findstr /B /L 0 ${SOURCES[1]} && (type nul > $TARGET) || (exit 1)"
        )
    else:
        env["PROOF_GENERATOR_COMMAND"] = (
            "$( $ICERUN $) ${SOURCES[0]} $UNITTEST_FLAGS > ${TARGETS[0]} 2>&1 ; echo $? > ${TARGETS[1]}"
        )
        env["PROOF_ANALYZER_COMMAND"] = (
            "if $$(exit $$(cat ${SOURCES[1]})) ; then touch $TARGET ; else cat ${SOURCES[0]}; exit 1 ; fi"
        )

    # TODO: Condition this on verbosity
    env["PROOF_GENERATOR_COMSTR"] = "Running test ${SOURCES[0]}"
    env["PROOF_ANALYZER_COMSTR"] = "Analyzing test results in ${SOURCES[1]}"
