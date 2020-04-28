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


def generate_test_execution_aliases(env, test):

    installed = [test]
    if env.get("AUTO_INSTALL_ENABLED", False) and env.GetAutoInstalledFiles(test):
        installed = env.GetAutoInstalledFiles(test)

    target_name = os.path.basename(installed[0].get_path())
    command = env.Command(
        target="#+{}".format(target_name),
        source=installed,
        action="${SOURCES[0]} $UNITTEST_FLAGS",
        NINJA_POOL="console",
    )

    env.Alias("test-execution-aliases", command)
    for source in test.sources:
        source_base_name = os.path.basename(source.get_path())
        # Strip suffix
        dot_idx = source_base_name.rfind(".")
        suffix = source_base_name[dot_idx:]
        if suffix in env["TEST_EXECUTION_SUFFIX_BLACKLIST"]:
            continue

        source_name = source_base_name[:dot_idx]
        if target_name == source_name:
            continue

        source_command = env.Command(
            target="#+{}".format(source_name),
            source=installed,
            action="${SOURCES[0]} -fileNameFilter $TEST_SOURCE_FILE_NAME $UNITTEST_FLAGS",
            TEST_SOURCE_FILE_NAME=source_name,
            NINJA_POOL="console",
        )

        env.Alias("test-execution-aliases", source_command)


def exists(env):
    return True


def generate(env):
    # Used for Ninja generator to collect the test execution aliases
    env.Alias("test-execution-aliases")
    env.AddMethod(generate_test_execution_aliases, "GenerateTestExecutionAliases")

    env["TEST_EXECUTION_SUFFIX_BLACKLIST"] = env.get(
        "TEST_EXECUTION_SUFFIX_BLACKLIST", [".in"]
    )

    # TODO: Remove when the new ninja generator is the only supported generator
    env["_NINJA_NO_TEST_EXECUTION"] = True
