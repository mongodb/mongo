"""Generate a docker compose configuration and all necessary infrastructure."""

import os
import sys
from buildscripts.resmokelib.errors import InvalidMatrixSuiteError, RequiresForceRemove
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.testing.docker_cluster_image_builder import DockerComposeImageBuilder

_HELP = """
Generate a docker compose configuration and all necessary infrastructure -- including base images.
"""
_COMMAND = "generate-docker-compose"


class GenerateDockerCompose(Subcommand):
    """Generate docker compose configuration and infrastructure."""

    def __init__(self, antithesis_suite_name, build_base_images, tag, in_evergreen):
        """
        Constructor for GenerateDockerCompose subcommand.

        :param antithesis_suite_name: The antithesis suite to generate a docker compose configuration for.
        :param build_base_images: Whether to build the base images or not.
        :param tag: The tag to use for the docker images and docker-compose file.
        :param in_evergreen: Whether this is running in Evergreen or not.
        """
        self._antithesis_suite_name = antithesis_suite_name
        self._build_base_images = build_base_images
        self._tag = tag
        self._in_evergreen = in_evergreen

    def execute(self) -> None:
        """
        Generate docker compose configuration and infrastructure.

        :return: None
        """
        try:
            image_builder = DockerComposeImageBuilder(self._tag, self._in_evergreen)
            if self._antithesis_suite_name:
                image_builder.build_config_image(self._antithesis_suite_name)
            if self._build_base_images:
                image_builder.build_base_images()
            if self._build_base_images and self._antithesis_suite_name:
                success_message = f"""
                Successfully generated docker compose configuration and built required base images.

                You can run the following command to verify that this docker compose configuration works:
                `cd antithesis/antithesis_config/{self._antithesis_suite_name} && bash run_suite.sh`
                """
                print(success_message)

        except RequiresForceRemove as exc:
            print(exc)
            sys.exit(2)
        except AssertionError as exc:
            print(exc)
            sys.exit(3)
        except InvalidMatrixSuiteError as exc:
            print(exc)
            sys.exit(4)
        except Exception as exc:
            raise Exception(
                "Something unexpected happened while building antithesis images.") from exc


class GenerateDockerComposePlugin(PluginInterface):
    """Generate docker compose configuration and infrastructure."""

    def add_subcommand(self, subparsers):
        """
        Add 'generate-docker-compose' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        parser = subparsers.add_parser(_COMMAND, help=_HELP)
        parser.add_argument("-t", "--tag", dest="tag", metavar="TAG", default="local-development",
                            help="Build base images needed for the docker compose configuration.")
        parser.add_argument("-s", "--skip-base-image-build", dest="skip_base_image_build",
                            default=False, action="store_true",
                            help="Skip building images for the docker compose configuration.")
        parser.add_argument(
            "--in-evergreen", dest="in_evergreen", default=False, action="store_true",
            help="If this is running in Evergreen, certain artifacts are expected to already exist."
        )
        parser.add_argument(
            nargs="?", dest="antithesis_suite", metavar="SUITE", help=
            ("Antithesis Matrix Suite file from the resmokeconfig/matrix_suites/mappings directory."
             " Use the basename without the .yml extension. If empty, only base images will be built."
             ))

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

        build_base_images = parsed_args.skip_base_image_build is False

        return GenerateDockerCompose(parsed_args.antithesis_suite, build_base_images,
                                     parsed_args.tag, parsed_args.in_evergreen)
