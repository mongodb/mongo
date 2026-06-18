"""Subcommand for multiversion config."""

import argparse
from typing import Optional

import yaml
from pydantic import BaseModel

from buildscripts.resmokelib import configure_resmoke
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
    * last_patch_version: Last patch release version (e.g. '8.3.1' or '8.3.1-rc1010'),
      derived from git tag history. Omitted when not resolvable.
    * last_patch_fcv: FCV derived from the last patch release (e.g. '8.3').
      Omitted when not resolvable.
    """

    last_versions: list[str]
    requires_fcv_tag: str
    requires_fcv_tag_lts: str
    requires_fcv_tag_continuous: str
    last_lts_fcv: str
    last_continuous_fcv: str
    last_patch_version: Optional[str] = None
    last_patch_fcv: Optional[str] = None


class MultiversionConfigSubcommand(Subcommand):
    """Subcommand for discovering multiversion configuration."""

    def __init__(self, options: dict) -> None:
        self.config_file_output = options["config_file_output"]
        self.include_last_patch = options.get("include_last_patch", False)

    def execute(self):
        """Execute the subcommand."""
        mv_config = self.determine_multiversion_config(include_last_patch=self.include_last_patch)
        yaml_output = yaml.safe_dump(mv_config.dict(exclude_none=True))
        print(yaml_output)

        if self.config_file_output:
            with open(self.config_file_output, "w") as file:
                file.write(yaml_output)

    @staticmethod
    def determine_multiversion_config(include_last_patch: bool = False) -> MultiversionConfig:
        """Discover the current multiversion configuration.

        :param include_last_patch: When True, resolve and include the last patch
          release fields (last_patch_version, last_patch_fcv). When False, those
          fields are left unset and omitted from the output.
        """
        from buildscripts.resmokelib import multiversionconstants

        version_constants = multiversionconstants.version_constants
        multiversion_service = multiversionconstants.multiversion_service
        last_patch_version = (
            multiversion_service.get_last_patch_version() if include_last_patch else None
        )
        last_patch_fcv = multiversion_service.get_last_patch_fcv() if include_last_patch else None
        return MultiversionConfig(
            last_versions=multiversion_service.get_last_versions(),
            requires_fcv_tag=version_constants.get_fcv_tag_list(),
            requires_fcv_tag_lts=version_constants.get_lts_fcv_tag_list(),
            requires_fcv_tag_continuous=version_constants.get_continuous_fcv_tag_list(),
            last_lts_fcv=version_constants.get_last_lts_fcv(),
            last_continuous_fcv=version_constants.get_last_continuous_fcv(),
            last_patch_version=last_patch_version,
            last_patch_fcv=last_patch_fcv,
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

        parser.add_argument(
            "--include-last-patch",
            action="store_true",
            default=False,
            help=(
                "Resolve and include the last patch release tag from git history "
                "(adds last_patch_version and last_patch_fcv to the output)."
            ),
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
