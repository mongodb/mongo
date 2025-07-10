"""Subcommand for multiversion config."""

import argparse
from typing import List, Optional

import yaml
from pydantic import BaseModel

from buildscripts.resmokelib import config, configure_resmoke
from buildscripts.resmokelib.multiversion.multiversion_service import (
    MongoReleases,
    MongoVersion,
    MultiversionService,
)
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

MULTIVERSION_SUBCOMMAND = "multiversion-config"


class MultiversionConfig(BaseModel):
    """
    Multiversion Configuration.

    * last_versions: List of which "last" versions that should be tests against (lts, continuous).
    * requires_fcv_tag: Which required_fcv tags should be used by default.
    * requires_fcv_tag_lts: Which requires_fcv tags should be used when running in LTS mode.
    * requires_fcv_tag_continuous: Which required_fcv tags should be used when running in continuous
      mode.
    * last_lts_fcv: LTS version that should be tested against.
    * last_continuous_fcv: Continuous version that should be tested against.
    """

    last_versions: List[str]
    requires_fcv_tag: str
    requires_fcv_tag_lts: str
    requires_fcv_tag_continuous: str
    last_lts_fcv: str
    last_continuous_fcv: str


class MultiversionConfigSubcommand(Subcommand):
    """Subcommand for discovering multiversion configuration."""

    def __init__(self, options: dict) -> None:
        self.config_file_output = options["config_file_output"]

    def execute(self):
        """Execute the subcommand."""
        mv_config = self.determine_multiversion_config()
        yaml_output = yaml.safe_dump(mv_config.dict())
        print(yaml_output)

        if self.config_file_output:
            with open(self.config_file_output, "w") as file:
                file.write(yaml_output)

    @staticmethod
    def determine_multiversion_config() -> MultiversionConfig:
        """Discover the current multiversion configuration."""
        from buildscripts.resmokelib import multiversionconstants

        multiversion_service = MultiversionService(
            mongo_version=MongoVersion.from_yaml_file(config.MONGO_VERSION_FILE),
            mongo_releases=MongoReleases.from_yaml_file(config.RELEASES_FILE),
        )
        version_constants = multiversion_service.calculate_version_constants()
        return MultiversionConfig(
            last_versions=multiversionconstants.OLD_VERSIONS,
            requires_fcv_tag=version_constants.get_fcv_tag_list(),
            requires_fcv_tag_lts=version_constants.get_lts_fcv_tag_list(),
            requires_fcv_tag_continuous=version_constants.get_continuous_fcv_tag_list(),
            last_lts_fcv=version_constants.get_last_lts_fcv(),
            last_continuous_fcv=version_constants.get_last_continuous_fcv(),
        )


class MultiversionPlugin(PluginInterface):
    """Multiversion plugin."""

    def add_subcommand(self, subparsers: argparse._SubParsersAction) -> None:
        """
        Add parser options for this plugin.

        :param subparsers: argparse subparsers
        """
        parser = subparsers.add_parser(
            MULTIVERSION_SUBCOMMAND, help="Display configuration for multiversion testing"
        )

        parser.add_argument(
            "--config-file-output",
            "-f",
            action="store",
            type=str,
            default=None,
            help="File to write the multiversion config to.",
        )

    def parse(
        self,
        subcommand: str,
        parser: argparse.ArgumentParser,
        parsed_args: dict,
        should_configure_otel=True,
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
        if subcommand == MULTIVERSION_SUBCOMMAND:
            configure_resmoke.detect_evergreen_config(parsed_args)
            configure_resmoke.validate_and_update_config(parser, parsed_args, should_configure_otel)
            return MultiversionConfigSubcommand(parsed_args)
        return None
