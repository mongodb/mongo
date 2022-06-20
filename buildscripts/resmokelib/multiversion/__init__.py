"""Subcommand for multiversion config."""
from typing import List, Optional

import yaml
from pydantic import BaseModel

from buildscripts.resmokelib import configure_resmoke
from buildscripts.resmokelib.multiversion.multiversion_service import (MongoReleases, MongoVersion,
                                                                       MultiversionService)
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

    def execute(self):
        """Execute the subcommand."""
        mv_config = self.determine_multiversion_config()
        print(yaml.safe_dump(mv_config.dict()))

    @staticmethod
    def determine_multiversion_config() -> MultiversionConfig:
        """Discover the current multiversion configuration."""
        from buildscripts.resmokelib import multiversionconstants
        multiversion_service = MultiversionService(
            mongo_version=MongoVersion.from_yaml_file(multiversionconstants.MONGO_VERSION_YAML),
            mongo_releases=MongoReleases.from_yaml_file(multiversionconstants.RELEASES_YAML),
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

    def add_subcommand(self, subparsers) -> None:
        """
        Add parser options for this plugin.

        :param subparsers: argparse subparsers
        """
        subparsers.add_parser(MULTIVERSION_SUBCOMMAND,
                              help="Display configuration for multiversion testing")

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
        if subcommand == MULTIVERSION_SUBCOMMAND:
            return MultiversionConfigSubcommand()
        return None
