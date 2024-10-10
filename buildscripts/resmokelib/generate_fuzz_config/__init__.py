"""Generate mongod.conf and mongos.conf using config fuzzer."""

import os.path
import shutil

from buildscripts.resmokelib import config, mongo_fuzzer_configs, utils
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand

_HELP = """
Generate a mongod.conf and mongos.conf using config fuzzer.
"""
_COMMAND = "generate-fuzz-config"


class GenerateFuzzConfig(Subcommand):
    """Interact with generating fuzz config."""

    def __init__(self, template_path, output_path, mongod_mode, mongos_mode, seed):
        """Constructor."""
        self._template_path = template_path
        self._output_path = output_path
        self._mongod_mode = mongod_mode
        self._mongos_mode = mongos_mode
        self._seed = seed

    def _generate_mongod_config(self) -> None:
        filename = "mongod.conf"
        output_file = os.path.join(self._output_path, filename)
        user_param = utils.dump_yaml({})
        set_parameters, wt_engine_config, wt_coll_config, wt_index_config, encryption_config = (
            mongo_fuzzer_configs.fuzz_mongod_set_parameters(
                self._mongod_mode, self._seed, user_param
            )
        )
        set_parameters = utils.load_yaml(set_parameters)
        # This is moved from Jepsen mongod.conf to have only one setParameter key value pair.
        set_parameters["enableTestCommands"] = True
        set_parameters["testingDiagnosticsEnabled"] = True
        conf = {
            "setParameter": set_parameters,
            "storage": {
                "wiredTiger": {
                    "engineConfig": {"configString": wt_engine_config},
                    "collectionConfig": {"configString": wt_coll_config},
                    "indexConfig": {"configString": wt_index_config},
                }
            },
        }
        if encryption_config:
            # Convert empty string to True for config file compatibility.
            # Resmoke uses an empty string to indicate a flag argument with no value,
            # but the mongod configuration file expects a boolean for enableEncryption.
            # https://www.mongodb.com/docs/manual/reference/configuration-options/#security-options
            if encryption_config.get("enableEncryption") == "":
                encryption_config["enableEncryption"] = True
            conf["security"] = encryption_config
        if self._template_path is not None:
            try:
                shutil.copy(os.path.join(self._template_path, filename), output_file)
            except shutil.SameFileError:
                pass

        fuzz_config = utils.dump_yaml(conf)
        with open(output_file, "a") as file:
            file.write(fuzz_config)
            file.write("\n")

    def _generate_mongos_config(self) -> None:
        filename = "mongos.conf"
        output_file = os.path.join(self._output_path, filename)
        user_param = utils.dump_yaml({})
        set_parameters = mongo_fuzzer_configs.fuzz_mongos_set_parameters(self._seed, user_param)
        set_parameters = utils.load_yaml(set_parameters)
        conf = {"setParameter": set_parameters}
        if self._template_path is not None:
            try:
                shutil.copy(os.path.join(self._template_path, filename), output_file)
            except shutil.SameFileError:
                pass
            except FileNotFoundError:
                print("There is no mongos template in the path, skip generating mongos.conf.")
                return

        fuzz_config = utils.dump_yaml(conf)
        with open(output_file, "a") as file:
            file.write(fuzz_config)
            file.write("\n")

    def execute(self) -> None:
        """
        Generate mongod.conf and mongos.conf.

        :return: None
        """
        self._generate_mongod_config()
        self._generate_mongos_config()


class GenerateFuzzConfigPlugin(PluginInterface):
    """Interact with generating fuzz config."""

    def add_subcommand(self, subparsers):
        """
        Add 'generate-fuzz-config' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        parser = subparsers.add_parser(_COMMAND, help=_HELP)
        parser.add_argument(
            "--template",
            "-t",
            type=str,
            required=False,
            help="Path to templates to append config-fuzzer-generated parameters.",
        )
        parser.add_argument(
            "--output", "-o", type=str, required=True, help="Path to the output file."
        )
        parser.add_argument(
            "--fuzzMongodConfigs",
            dest="fuzz_mongod_configs",
            help="Randomly chooses mongod parameters that were not specified. Use 'stress' to fuzz "
            "all configs including stressful storage configurations that may significantly "
            "slow down the server. Use 'normal' to only fuzz non-stressful configurations. ",
            metavar="MODE",
            choices=("normal", "stress"),
        )
        parser.add_argument(
            "--fuzzMongosConfigs",
            dest="fuzz_mongos_configs",
            help="Randomly chooses mongos parameters that were not specified",
            metavar="MODE",
            choices=("normal",),
        )
        parser.add_argument(
            "--configFuzzSeed",
            dest="config_fuzz_seed",
            metavar="PATH",
            help="Sets the seed used by mongod and mongos config fuzzers",
        )

        parser.add_argument(
            "--disableEncryptionFuzzing",
            dest="disable_encryption_fuzzing",
            action="store_true",
            help="Disables the fuzzing that sometimes enables the encrypted storage engine.",
        )

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """
        Return the GenerateFuzzConfig subcommand for execution.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """

        if subcommand != _COMMAND:
            return None

        config.DISABLE_ENCRYPTION_FUZZING = parsed_args.disable_encryption_fuzzing
        return GenerateFuzzConfig(
            parsed_args.template,
            parsed_args.output,
            parsed_args.fuzz_mongod_configs,
            parsed_args.fuzz_mongos_configs,
            parsed_args.config_fuzz_seed,
        )
