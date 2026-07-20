"""Subcommands for test discovery."""

import argparse
from typing import Optional

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
    tests: list[str]


class TestDiscoverySubcommand(Subcommand):
    """Subcommand for test discovery."""

    def __init__(self, suite_names: list[str]) -> None:
        """
        Initialize the subcommand.

        :param suite_names: Suites to discover.
        """
        self.suite_names = suite_names
        self.suite_config = suitesconfig

    def execute(self):
        """Execute the subcommand."""
        test_lists = [
            self.gather_tests(self.suite_config.get_suite(suite_name)).dict()
            for suite_name in self.suite_names
        ]

        # A single suite keeps the historical single-document output; multiple suites
        # are emitted as a multi-document YAML stream in the order they were requested.
        print(yaml.safe_dump_all(test_lists))

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
        parser.add_argument(
            "--suite",
            metavar="SUITE",
            action="append",
            required=True,
            help=(
                "Suite to run against. May be repeated to discover several suites in one"
                " invocation; the output is then a multi-document YAML stream with one"
                " document per suite, in the requested order."
            ),
        )
        parser.add_argument(
            "--skipTestsCoveredByMoreComplexSuites",
            dest="skip_tests_covered_by_more_complex_suites",
            action="store_true",
            help=(
                "Excludes tests from running on some suite_A if a more complex"
                " suite_A_B will also run the same tests."
            ),
        )
        parser.add_argument(
            "--includeFullyDisabledFeatureTests",
            dest="include_fully_disabled_feature_tests",
            action="store_true",
            help=(
                "Include tests tagged with features that are in fully_disabled_feature_flags.yml."
            ),
        )

        parser = subparsers.add_parser(
            SUITECONFIG_SUBCOMMAND, help="Display configuration of a test suite."
        )
        parser.add_argument("--suite", metavar="SUITE", help="Suite to run against.")

    def parse(
        self,
        subcommand: str,
        parser: argparse.ArgumentParser,
        parsed_args: dict,
        should_configure_otel: bool = True,
        **kwargs,
    ) -> Optional[Subcommand]:
        """
        Resolve command-line options to a Subcommand or None.

        :param subcommand: equivalent to parsed_args.command.
        :param parser: parser used.
        :param parsed_args: output of parsing.
        :param kwargs: additional args.
        :return: None or a Subcommand.
        """
        if subcommand == TEST_DISCOVERY_SUBCOMMAND:
            configure_resmoke.validate_and_update_config(parser, parsed_args, should_configure_otel)
            return TestDiscoverySubcommand(parsed_args["suite"])
        if subcommand == SUITECONFIG_SUBCOMMAND:
            configure_resmoke.validate_and_update_config(parser, parsed_args, should_configure_otel)
            return SuiteConfigSubcommand(parsed_args["suite"])
        return None
