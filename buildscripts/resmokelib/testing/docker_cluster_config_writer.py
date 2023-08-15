"""Create necessary Docker files and topology for a suite to be able to use in Antithesis."""

import sys
import os
import re
import shutil

from jinja2 import Environment, FileSystemLoader

from buildscripts.resmokelib.core import programs
from buildscripts.resmokelib.errors import InvalidMatrixSuiteError, RequiresForceRemove
from buildscripts.resmokelib.suitesconfig import get_suite
from buildscripts.resmokelib.testing.fixtures import _builder

MONGOS_PORT = 27017
MONGOD_PORT = 27018
CONFIG_PORT = 27019


def get_antithesis_base_suite_fixture(antithesis_suite_name) -> None:
    """
    Get the base suite fixture to use for generating docker compose configuration.

    :param antithesis_suite_name: The antithesis suite to find the base suite fixture for.
    """
    antithesis_suite = get_suite(antithesis_suite_name)
    if not antithesis_suite.is_matrix_suite() or not antithesis_suite.is_antithesis_suite():
        raise InvalidMatrixSuiteError(
            f"The specified suite is not an antithesis matrix suite: {antithesis_suite.get_name()}")

    antithesis_fixture = antithesis_suite.get_executor_config()["fixture"]["class"]
    if antithesis_fixture != "ExternalShardedClusterFixture":
        raise InvalidMatrixSuiteError(
            "Generating docker compose infrastructure for this external fixture is not yet supported"
        )

    antithesis_base_suite = antithesis_suite.get_executor_config()["fixture"]["original_suite_name"]
    base_suite_fixture = _builder.make_dummy_fixture(antithesis_base_suite)
    if base_suite_fixture.__class__.__name__ != "ShardedClusterFixture":
        raise InvalidMatrixSuiteError(
            "Generating docker compose infrastructure for this base suite fixture is not yet supported.{}"
        )

    return base_suite_fixture


class DockerClusterConfigWriter(object):
    """Create necessary files and topology for a suite to run with Antithesis."""

    def __init__(self, antithesis_suite_name, tag):
        """
        Initialize the class with the specified fixture.

        :param antithesis_suite_name: Suite we wish to generate files and topology for.
        :param tag: Tag to use for the docker compose configuration and/or base images.
        """
        self.ip_address = 1
        self.antithesis_suite_name = antithesis_suite_name
        self.fixture = get_antithesis_base_suite_fixture(antithesis_suite_name)
        self.tag = tag
        self.build_context = os.path.join(os.getcwd(),
                                          f"antithesis/antithesis_config/{antithesis_suite_name}")
        self.jinja_env = Environment(
            loader=FileSystemLoader(os.path.join(os.getcwd(), "antithesis/templates/")))

    def generate_docker_sharded_cluster_config(self):
        """
        Generate all necessary files and topology for the suite fixture.

        :return: None.
        """
        # Create volume directory structure
        self.create_volume_directories()
        # Create configsvr init scripts
        self.create_configsvr_init()
        # Create mongod init scripts
        self.create_mongod_init()
        # Create mongos init scripts
        self.create_mongos_init()
        # Create workload init
        self.write_workload_init()
        # Create docker-compose
        self.write_docker_compose()
        # Create dockerfile
        self.write_dockerfile()
        # Create run suite script
        self.write_run_suite_script()

    def write_docker_compose(self):
        """
        Write the docker-compose.yml file utilizing information from the suite fixture.

        :return: None.
        """
        with open(os.path.join(self.build_context, "docker-compose.yml"), 'w') as file:
            template = self.jinja_env.get_template("docker_compose_template.yml.jinja")
            file.write(
                template.render(
                    num_configsvr=self.fixture.configsvr_options.get("num_nodes", 1),
                    num_shard=self.fixture.num_shards, num_node_per_shard=self.fixture.
                    num_rs_nodes_per_shard, num_mongos=self.fixture.num_mongos, tag=self.tag,
                    get_and_increment_ip_address=self.get_and_increment_ip_address) + "\n")

    def write_workload_init(self):
        """
        Write the workload_init.py file to be Antithesis compatible.

        :return: None.
        """
        with open(os.path.join(self.build_context, "scripts/workload_init.py"), 'w') as file:
            template = self.jinja_env.get_template("workload_init_template.py.jinja")
            file.write(template.render() + "\n")

    def write_dockerfile(self):
        """
        Write the Dockerfile for the suite.

        :return: None.
        """
        with open(os.path.join(self.build_context, "Dockerfile"), 'w') as file:
            template = self.jinja_env.get_template("dockerfile_template.jinja")
            file.write(template.render() + "\n")

    def create_volume_directories(self):
        """
        Create the necessary volume directories for the Docker topology.

        :return: None.
        """
        paths = [
            self.build_context,
            os.path.join(self.build_context, "scripts"),
            os.path.join(self.build_context, "logs"),
            os.path.join(self.build_context, "data"),
            os.path.join(self.build_context, "debug")
        ]
        for p in paths:
            if os.path.exists(p):
                try:
                    shutil.rmtree(p)
                except Exception as exc:
                    exception_text = f"""
                    Could not remove directory due to old artifacts from a previous run.

                    Please remove this directory and try again -- you may need to force remove:
                    `{os.path.relpath(p)}`
                    """
                    raise RequiresForceRemove(exception_text) from exc

            os.makedirs(p)

    def get_and_increment_ip_address(self):
        """
        Increment and return ip_address attribute for this suite if it is between 0-24, else exit with error code 2.

        :return: ip_address.
        """
        if self.ip_address > 24:
            print(f"Exiting with code 2 -- ipv4_address exceeded 10.20.20.24: {self.ip_address}")
            sys.exit(2)
        self.ip_address += 1
        return self.ip_address

    def create_configsvr_init(self):
        """
        Create configsvr init scripts for all of the configsvr nodes for this suite fixture.

        :return: None.
        """
        for i, node in enumerate(self.fixture.configsvr.nodes):
            mongod_options = node.get_options()
            args = self.construct_mongod_args(mongod_options)
            self.write_node_init(f"configsvr{i}", args)

    def create_mongod_init(self):
        """
        Create mongod init scripts for all of the mongod nodes for this suite fixture.

        :return: None.
        """
        for s, shard in enumerate(self.fixture.shards):
            for i, node in enumerate(shard.nodes):
                mongod_options = node.get_options()
                args = self.construct_mongod_args(mongod_options)
                self.write_node_init(f"mongod{s*self.fixture.num_rs_nodes_per_shard+i}", args)

    def write_node_init(self, node_name, args):
        """
        Write init script for a node based on arguments and name provided.

        :param node_name: String with the name of the node to write init script for.
        :param args: List of arguments for initiating the current node.
        :return: None.
        """
        script_path = os.path.join(self.build_context, f"scripts/{node_name}_init.sh")
        with open(script_path, 'w') as file:
            template = self.jinja_env.get_template("node_init_template.sh.jinja")
            file.write(template.render(command=' '.join(args)) + "\n")

    def construct_mongod_args(self, mongod_options):
        """
        Return list of mongod args that are Antithesis compatible.

        :param mongod_options: Dictionary of options that mongod is initiated with.
        :return: List of mongod args.
        """
        d_args = ["mongod"]
        suite_set_parameters = mongod_options.get("set_parameters", {})
        self.update_mongod_for_antithesis(mongod_options, suite_set_parameters)
        programs._apply_set_parameters(d_args, suite_set_parameters)
        mongod_options.pop("set_parameters")
        programs._apply_kwargs(d_args, mongod_options)
        return d_args

    def update_mongod_for_antithesis(self, mongod_options, suite_set_parameters):
        """
        Add and remove certain options and params so mongod init is Antithesis compatible.

        :param mongod_options: Dictionary of options that mongod is initiated with.
        :param suite_set_parameters: Dictionary of parameters that need to be set for mongod init.
        :return: None.
        """
        suite_set_parameters["fassertOnLockTimeoutForStepUpDown"] = 0
        suite_set_parameters.pop("logComponentVerbosity", None)
        suite_set_parameters.pop("backtraceLogFile", None)
        mongod_options.pop("dbpath", None)
        if "configsvr" in mongod_options:
            mongod_options["port"] = CONFIG_PORT
        else:
            mongod_options["port"] = MONGOD_PORT
        mongod_options.update({
            "logpath": "/var/log/mongodb/mongodb.log", "bind_ip": "0.0.0.0", "oplogSize": "256",
            "wiredTigerCacheSizeGB": "1"
        })
        if "shardsvr" in mongod_options:
            s = int(re.search(r'\d+$', mongod_options["replSet"]).group())
            mongod_options["replSet"] = f"Shard{s}"

    def create_mongos_init(self):
        """
        Set up the creation of mongos init scripts.

        :return: None.
        """
        for m in range(self.fixture.num_mongos):
            mongos_options = self.fixture.mongos[m].get_options()
            args = self.construct_mongos_args(mongos_options)
            self.write_mongos_init(f"mongos{m}", args)

    def write_mongos_init(self, mongos_name, args):
        """
        Write the mongos init scripts utilizing information from the suite fixture.

        :param mongos_name: String with the name of the mongos to write init script for.
        :param args: List of arguments that need to be set for mongod init.
        :return: None.
        """
        with open(os.path.join(self.build_context, f"scripts/{mongos_name}_init.py"), 'w') as file:
            template = self.jinja_env.get_template("mongos_init_template.py.jinja")
            file.write(
                template.render(mongos_name=mongos_name, configsvr=self.fixture.configsvr,
                                shards=self.fixture.shards, MONGOS_PORT=MONGOS_PORT,
                                MONGOD_PORT=MONGOD_PORT, CONFIG_PORT=CONFIG_PORT, mongos_args=args,
                                get_replset_settings=self.get_replset_settings) + "\n")

    def construct_mongos_args(self, mongos_options):
        """
        Return list of mongos args that are Antithesis compatible.

        :param mongos_options: Dictionary of options that mongos is initiated with.
        :return: List of mongos args.
        """
        d_args = ["mongos"]
        self.update_mongos_for_antithesis(mongos_options)
        suite_set_parameters = mongos_options.get("set_parameters", {})
        programs._apply_set_parameters(d_args, suite_set_parameters)
        mongos_options.pop("set_parameters")
        programs._apply_kwargs(d_args, mongos_options)
        return d_args

    def update_mongos_for_antithesis(self, mongos_options):
        """
        Add and remove certain options and params so mongos init is Antithesis compatible.

        :param mongos_options: Dictionary of options that mongos is initiated with.
        :return: None.
        """
        members = [
            f"configsvr{i}:{CONFIG_PORT}"
            for i in range(self.fixture.configsvr_options.get("num_nodes", 1))
        ]
        mongos_options["configdb"] = f"config-rs/{','.join(members)}"
        mongos_options.pop("port", None)

    def get_replset_settings(self, replset):
        """
        Return dictionary of settings for a specific replset that are Antithesis compatible.

        :param replset: Replset that contains config options.
        :return: Dictionary of settings.
        """
        settings = {}
        if replset.replset_config_options.get("settings"):
            replset_settings = replset.replset_config_options["settings"]
            settings = replset_settings.to_storable_dict()["object_value"]
            settings.update({
                "electionTimeoutMillis": 2000, "heartbeatTimeoutSecs": 1, "chainingAllowed": False
            })
        return settings

    def write_run_suite_script(self):
        """
        Write the `run_suite.sh` file which starts up the docker cluster and runs a sanity check.

        This ensures that the configuration for the suite works as expected.

        :return: None.
        """
        with open(os.path.join(self.build_context, "run_suite.sh"), 'w') as file:
            template = self.jinja_env.get_template("run_suite_template.sh.jinja")
            file.write(template.render(suite=self.antithesis_suite_name) + "\n")
