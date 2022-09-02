# Copyright (C) 2020-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
"""Check that mongod's and mongos's Stable API commands are defined in IDL.

Call listCommands on mongod and mongos to assert they have the same set of commands in the given API
version, and assert all these commands are defined in IDL.
"""

import argparse
import logging
import os
import sys
from tempfile import TemporaryDirectory
from typing import Dict, List, Set

from pymongo import MongoClient

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), '../../..')))

# pylint: disable=wrong-import-position
from idl import syntax
from buildscripts.idl.lib import list_idls, parse_idl
from buildscripts.resmokelib import configure_resmoke
from buildscripts.resmokelib.logging import loggers
from buildscripts.resmokelib.testing.fixtures import interface
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.testing.fixtures.shardedcluster import ShardedClusterFixture
from buildscripts.resmokelib.testing.fixtures.standalone import MongoDFixture
# pylint: enable=wrong-import-position

LOGGER_NAME = 'check-idl-definitions'
LOGGER = logging.getLogger(LOGGER_NAME)


def is_test_or_third_party_idl(idl_path: str) -> bool:
    """Check if an IDL file is a test file or from a third-party library."""
    ignored_idls_subpaths = ["/idl/tests/", "unittest.idl", "/src/third_party/"]

    for file_name in ignored_idls_subpaths:
        if idl_path.find(file_name) != -1:
            return True

    return False


def get_command_definitions(api_version: str, directory: str,
                            import_directories: List[str]) -> Dict[str, syntax.Command]:
    """Get parsed IDL definitions of commands in a given API version."""

    LOGGER.info("Searching for command definitions in %s", directory)

    def gen():
        for idl_path in sorted(list_idls(directory)):
            if not is_test_or_third_party_idl(idl_path):
                for command in parse_idl(idl_path, import_directories).spec.symbols.commands:
                    if command.api_version == api_version:
                        yield command.command_name, command

    idl_commands = dict(gen())
    LOGGER.debug("Found %s IDL commands in API Version %s", len(idl_commands), api_version)
    return idl_commands


def list_commands_for_api(api_version: str, mongod_or_mongos: str, install_dir: str) -> Set[str]:
    """Get a list of commands in a given API version by calling listCommands."""
    assert mongod_or_mongos in ("mongod", "mongos")
    logging.info("Calling listCommands on %s", mongod_or_mongos)
    dbpath = TemporaryDirectory()
    fixturelib = FixtureLib()
    mongod_executable = os.path.join(install_dir, "mongod")
    mongos_executable = os.path.join(install_dir, "mongos")
    if mongod_or_mongos == "mongod":
        logger = loggers.new_fixture_logger("MongoDFixture", 0)
        logger.parent = LOGGER
        fixture: interface.Fixture = fixturelib.make_fixture(
            "MongoDFixture", logger, 0, dbpath_prefix=dbpath.name,
            mongod_executable=mongod_executable, mongod_options={"set_parameters": {}})
    else:
        logger = loggers.new_fixture_logger("ShardedClusterFixture", 0)
        logger.parent = LOGGER
        fixture = fixturelib.make_fixture(
            "ShardedClusterFixture", logger, 0, dbpath_prefix=dbpath.name,
            mongos_executable=mongos_executable, mongod_executable=mongod_executable,
            mongod_options={"set_parameters": {}})

    fixture.setup()
    fixture.await_ready()

    try:
        client = MongoClient(fixture.get_driver_connection_url())
        reply = client.admin.command('listCommands')
        commands = {
            name
            for name, info in reply['commands'].items() if api_version in info['apiVersions']
        }
        logging.info("Found %s commands in API Version %s on %s", len(commands), api_version,
                     mongod_or_mongos)
        return commands
    finally:
        fixture.teardown()


def assert_command_sets_equal(api_version: str, command_sets: Dict[str, Set[str]]):
    """Check that all sources have the same set of commands for a given API version."""
    LOGGER.info("Comparing %s command sets", len(command_sets))
    for name, commands in command_sets.items():
        LOGGER.info("--------- %s API Version %s commands --------------", name, api_version)
        for command in sorted(commands):
            LOGGER.info("%s", command)

    LOGGER.info("--------------------------------------------")
    it = iter(command_sets.items())
    name, commands = next(it)
    for other_name, other_commands in it:
        if commands != other_commands:
            if commands - other_commands:
                LOGGER.error("%s has commands not in %s: %s", name, other_name,
                             commands - other_commands)
            if other_commands - commands:
                LOGGER.error("%s has commands not in %s: %s", other_name, name,
                             other_commands - commands)
            raise AssertionError(
                f"{name} and {other_name} have different commands in API Version {api_version}")


def remove_skipped_commands(command_sets: Dict[str, Set[str]]):
    """Remove skipped commands from command_sets."""
    skipped_commands = {
        "testDeprecation",
        "testVersions1And2",
        "testRemoval",
        "testDeprecationInVersion2",
        # Idl specifies the command_name as hello.
        "isMaster",
    }

    for key in command_sets.keys():
        command_sets[key].difference_update(skipped_commands)


def main():
    """Run the script."""
    arg_parser = argparse.ArgumentParser(description=__doc__)
    arg_parser.add_argument("--include", type=str, action="append",
                            help="Directory to search for IDL import files")
    arg_parser.add_argument("--install-dir", dest="install_dir", required=True,
                            help="Directory to search for MongoDB binaries")
    arg_parser.add_argument("-v", "--verbose", action="count", help="Enable verbose logging")
    arg_parser.add_argument("api_version", metavar="API_VERSION", help="API Version to check")
    args = arg_parser.parse_args()

    class FakeArgs:
        """Fake argparse.Namespace-like class to pass arguments to _update_config_vars."""

        def __init__(self):
            self.INSTALL_DIR = args.install_dir  # pylint: disable=invalid-name
            self.command = ""

    # pylint: disable=protected-access
    configure_resmoke._update_config_vars(FakeArgs())
    configure_resmoke._set_logging_config()

    # Configure Fixture logging.
    loggers.configure_loggers()
    loggers.new_job_logger(sys.argv[0], 0)
    logging.basicConfig(level=logging.WARNING)
    logging.getLogger(LOGGER_NAME).setLevel(logging.DEBUG if args.verbose else logging.INFO)

    command_sets = {}
    command_sets["mongod"] = list_commands_for_api(args.api_version, "mongod", args.install_dir)
    command_sets["mongos"] = list_commands_for_api(args.api_version, "mongos", args.install_dir)
    command_sets["idl"] = set(get_command_definitions(args.api_version, os.getcwd(), args.include))
    remove_skipped_commands(command_sets)
    assert_command_sets_equal(args.api_version, command_sets)


if __name__ == "__main__":
    main()
