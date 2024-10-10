"""Subcommands for test discovery."""

from typing import List, Optional

import yaml
from pydantic import BaseModel

from buildscripts.resmokelib import configure_resmoke, suitesconfig
from buildscripts.resmokelib.multiversion.multiversion_service import (
    MongoReleases,
    MongoVersion,
    MultiversionService,
)
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.testing.suite import Suite

__all__ = ["MultiversionService", "MongoReleases", "MongoVersion"]

TEST_DISCOVERY_SUBCOMMAND = "test-discovery"
SUITECONFIG_SUBCOMMAND = "suiteconfig"


class SuiteTestList(BaseModel):
    """Collection of tests belonging to a suite."""

    suite_name: str
    tests: List[str]


class TestDiscoverySubcommand(Subcommand):
    """Subcommand for test discovery."""

    def __init__(self, suite_name: str) -> None:
        """
        Initialize the subcommand.

        :param suite_name: Suite to discover.
        """
        self.suite_name = suite_name
        self.suite_config = suitesconfig

    def execute(self):
        """Execute the subcommand."""
        suite = self.suite_config.get_suite(self.suite_name)
        test_list = self.gather_tests(suite)

        print(yaml.safe_dump(test_list.dict()))

    @staticmethod
    def gather_tests(suite: Suite) -> SuiteTestList:
        """
        Find all the tests that belong to the given suite.

        :param suite: Suite to query.
        :return: List of tests belonging to the suite.
        """
        test_list = []
        for tests in suite.tests:
            # `tests` could return individual tests or lists of tests, we need to handle both.
            if isinstance(tests, list):
                test_list.extend(tests)
            else:
                test_list.append(tests)

        return SuiteTestList(
            suite_name=suite.get_display_name(),
            tests=test_list,
        )


class SuiteConfigSubcommand(Subcommand):
    """Subcommand for discovering configuration of a suite."""

    def __init__(self, suite_name: str) -> None:
        """
        Initialize the subcommand.

        :param suite_name: Suite to discover.
        """
        self.suite_name = suite_name
        self.suite_config = suitesconfig

    def execute(self):
        """Execute the subcommand."""
        suite = self.suite_config.get_suite(self.suite_name)
        print(yaml.safe_dump(suite.get_config()))


class DiscoveryPlugin(PluginInterface):
    """Test discovery plugin."""

    def add_subcommand(self, subparsers) -> None:
        """
        Add parser options for this plugin.

        :param subparsers: argparse subparsers
        """
        parser = subparsers.add_parser(
            TEST_DISCOVERY_SUBCOMMAND, help="Discover what tests are run by a suite."
        )
        parser.add_argument("--suite", metavar="SUITE", help="Suite to run against.")

        parser = subparsers.add_parser(
            SUITECONFIG_SUBCOMMAND, help="Display configuration of a test suite."
        )
        parser.add_argument("--suite", metavar="SUITE", help="Suite to run against.")

    def parse(self, subcommand, parser, parsed_args, **kwargs) -> Optional[Subcommand]:
        """
        Resolve command-line options to a Subcommand or None.

        :param subcommand: equivalent to parsed_args.command.
        :param parser: parser used.
        :param parsed_args: output of parsing.
        :param kwargs: additional args.
        :return: None or a Subcommand.
        """
        configure_resmoke.validate_and_update_config(parser, parsed_args)
        if subcommand == TEST_DISCOVERY_SUBCOMMAND:
            return TestDiscoverySubcommand(parsed_args.suite)
        if subcommand == SUITECONFIG_SUBCOMMAND:
            return SuiteConfigSubcommand(parsed_args.suite)
        return None
