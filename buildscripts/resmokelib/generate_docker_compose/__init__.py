"""Generate a docker compose configuration and all necessary infrastructure."""

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.testing.docker_cluster_config_writer import DockerClusterConfigWriter
from buildscripts.resmokelib.testing.fixtures import _builder

_HELP = """
Generate a docker compose configuration and all necessary infrastructure.
"""
_COMMAND = "generate-docker-compose"


class GenerateDockerCompose(Subcommand):
    """Generate docker compose configuration and infrastructure."""

    def __init__(self, suite_name):
        """Constructor."""
        self._fixture = _builder.make_dummy_fixture(suite_name)
        self._suite_name = suite_name

    def execute(self) -> None:
        """
        Generate docker compose configuration and infrastructure.

        :return: None
        """
        if self._fixture.__class__.__name__ == "ShardedClusterFixture":
            DockerClusterConfigWriter(self._suite_name,
                                      self._fixture).generate_docker_sharded_cluster_config()
        else:
            print("Generating docker compose infrastructure for this fixture is not yet supported.")
            exit(1)


class GenerateDockerComposePlugin(PluginInterface):
    """Generate docker compose configuration and infrastructure."""

    def add_subcommand(self, subparsers):
        """
        Add 'generate-docker-compose' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        parser = subparsers.add_parser(_COMMAND, help=_HELP)
        parser.add_argument(
            "--suite", dest="suite", metavar="SUITE",
            help=("Suite file from the resmokeconfig/suites/ directory."
                  " Use the basename without the .yml extension."))

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return the GenerateDockerCompose subcommand for execution.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """

        if subcommand != _COMMAND:
            return None

        return GenerateDockerCompose(parsed_args.suite)
