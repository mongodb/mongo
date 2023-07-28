"""Create necessary Docker files and topology for a suite to be able to use in Antithesis."""

import sys
import os
import re
import shutil

from buildscripts.resmokelib import config
from buildscripts.resmokelib.core import programs
from jinja2 import Environment, FileSystemLoader

MONGOS_PORT = 27017
MONGOD_PORT = 27018
CONFIG_PORT = 27019


class DockerClusterConfigWriter(object):
    """Create necessary files and topology for a suite to run with Antithesis."""

    def __init__(self, fixture):
        """
        Initialize the class with the specified fixture.

        :param fixture: Fixture for the suite we wish to generate files and topology for.
        """
        self.ip_address = 1
        self.fixture = fixture
        self.suite_path = os.path.join(os.getcwd(),
                                       f"antithesis/antithesis_config/{config.SUITE_FILES.pop()}")
        self.jinja_env = Environment(
            loader=FileSystemLoader(os.path.join(os.getcwd(), "antithesis/templates/")))

    def generate_docker_sharded_cluster_config(self):
        """
        Generate all necessary files and topology for the suite fixture.

        :return: None.
        """
        # Create directory structure
        self.create_directory_structure()
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

    def write_docker_compose(self):
        """
        Write the docker-compose.yml file utilizing information from the suite fixture.

        :return: None.
        """
        with open(os.path.join(self.suite_path, "docker-compose.yml"), 'w') as file:
            template = self.jinja_env.get_template("docker_compose_template.yml.jinja")
            file.write(
                template.render(
                    num_configsvr=self.fixture.configsvr_options.get(
                        "num_nodes", 1), num_shard=self.fixture.num_shards, num_node_per_shard=self.
                    fixture.num_rs_nodes_per_shard, num_mongos=self.fixture.num_mongos,
                    get_and_increment_ip_address=self.get_and_increment_ip_address) + "\n")

    def write_workload_init(self):
        """
        Write the workload_init.py file to be Antithesis compatible.

        :return: None.
        """
        with open(os.path.join(self.suite_path, "scripts/workload_init.py"), 'w') as file:
            template = self.jinja_env.get_template("workload_init_template.py.jinja")
            file.write(template.render() + "\n")

    def write_dockerfile(self):
        """
        Write the Dockerfile for the suite.

        :return: None.
        """
        with open(os.path.join(self.suite_path, "Dockerfile"), 'w') as file:
            template = self.jinja_env.get_template("dockerfile_template.jinja")
            file.write(template.render() + "\n")

    def create_directory_structure(self):
        """
        Create the necessary directories for Docker topology. These will overwrite any existing topology for this suite.

        :return: None.
        """
        paths = [
            self.suite_path,
            os.path.join(self.suite_path, "scripts"),
            os.path.join(self.suite_path, "logs"),
            os.path.join(self.suite_path, "data"),
            os.path.join(self.suite_path, "debug")
        ]
        for p in paths:
            if os.path.exists(p):
                shutil.rmtree(p)
            os.makedirs(p)

    def get_and_increment_ip_address(self):
        """
        Increment and return ip_address attribute for this suite if it is between 0-24, else exit with error code 2.

        :return: ip_address.
        """
        if self.ip_address > 24:
            self._resmoke_logger.info("ipv4_address exceeded 10.20.20.24. Exiting with code: 2")
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
        script_path = os.path.join(self.suite_path, f"scripts/{node_name}_init.sh")
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
        mongod_options.pop("port", None)
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
        with open(os.path.join(self.suite_path, f"scripts/{mongos_name}_init.py"), 'w') as file:
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
