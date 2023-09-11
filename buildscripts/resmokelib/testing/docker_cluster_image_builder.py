import os
from shlex import quote
import shutil
import subprocess
import sys

import git
import yaml
from buildscripts.resmokelib import config
from buildscripts.resmokelib.errors import RequiresForceRemove


def build_images(suite_name, fixture_instance):
    """Build images needed to run the resmoke suite against docker containers."""
    image_builder = DockerComposeImageBuilder(suite_name, fixture_instance)
    if "config" in config.DOCKER_COMPOSE_BUILD_IMAGES:  # pylint: disable=unsupported-membership-test
        image_builder.build_config_image()
    if "mongo-binaries" in config.DOCKER_COMPOSE_BUILD_IMAGES:  # pylint: disable=unsupported-membership-test
        image_builder.build_mongo_binaries_image()
    if "workload" in config.DOCKER_COMPOSE_BUILD_IMAGES:  # pylint: disable=unsupported-membership-test
        image_builder.build_workload_image()
    if config.DOCKER_COMPOSE_BUILD_IMAGES:
        repro_command = f"""
        Built image(s): {config.DOCKER_COMPOSE_BUILD_IMAGES}

        SUCCESS - Run this suite against an External System Under Test (SUT) with the following command:
        `docker compose -f docker_compose/{suite_name}/docker-compose.yml run --rm workload {image_builder.get_resmoke_run_command()}`

        DISCLAIMER - Make sure you have built all images with the following command first:
        `buildscripts/resmoke.py run --suite {suite_name} --dockerComposeBuildImages workload,mongo-binaries,config`
        """
        print(repro_command)


class DockerComposeImageBuilder:
    """Build images needed to run a resmoke suite against a MongoDB Docker Container topology."""

    def __init__(self, suite_name, suite_fixture):
        """
        Constructs a `DockerComposeImageBuilder` which can build images locally and in CI.

        :param suite_name: The name of the suite we are building images for.
        :param suite_fixture: The fixture to base the `docker-compose.yml` generation off of.
        """
        self.suite_name = suite_name
        self.suite_fixture = suite_fixture
        self.tag = config.DOCKER_COMPOSE_TAG
        self.in_evergreen = config.DOCKER_COMPOSE_BUILD_ENV == "evergreen"

        # Build context constants
        self.DOCKER_COMPOSE_BUILD_CONTEXT = f"docker_compose/{self.suite_name}"

        self.WORKLOAD_BUILD_CONTEXT = "buildscripts/antithesis/base_images/workload"
        self.WORKLOAD_DOCKERFILE = f"{self.WORKLOAD_BUILD_CONTEXT}/Dockerfile"

        self.MONGO_BINARIES_BUILD_CONTEXT = "buildscripts/antithesis/base_images/mongo_binaries"
        self.MONGO_BINARIES_DOCKERFILE = f"{self.MONGO_BINARIES_BUILD_CONTEXT}/Dockerfile"

        # Artifact constants
        self.MONGODB_BINARIES_RELATIVE_DIR = "dist-test" if self.in_evergreen else "antithesis-dist-test"
        self.MONGO_BINARY = f"{self.MONGODB_BINARIES_RELATIVE_DIR}/bin/mongo"
        self.MONGOD_BINARY = f"{self.MONGODB_BINARIES_RELATIVE_DIR}/bin/mongod"
        self.MONGOS_BINARY = f"{self.MONGODB_BINARIES_RELATIVE_DIR}/bin/mongos"

        self.LIBVOIDSTAR_PATH = "/usr/lib/libvoidstar.so"
        self.MONGODB_DEBUGSYMBOLS = "mongo-debugsymbols.tgz"

        # Port suffix ranging from 1-24 is subject to fault injection while ports 130+ are safe.
        self.next_available_fault_enabled_ip = 2
        self.next_available_fault_disabled_ip = 130

    @staticmethod
    def get_resmoke_run_command() -> str:
        """Construct the supported resmoke `run` command to test against an external system under test."""
        # The supported `run` command should keep all of the same args except:
        # (1) it should remove the `--dockerComposeBuildImages` option and value
        # (2) it should add the `--externalSUT` flag
        command = sys.argv
        rm_index = command.index("--dockerComposeBuildImages")
        return ' '.join(command[0:rm_index] + command[rm_index + 2:] + ["--externalSUT"])

    def _add_docker_compose_configuration_to_build_context(self, build_context) -> None:
        """
        Create init scripts for all of the mongo{d,s} processes and a `docker-compose.yml` file.

        :param build_context: Filepath where the configuration is going to be set up.
        """

        def create_docker_compose_service(name, fault_injection, depends_on):
            """
            Create a service section of a docker-compose.yml for a service with this name.

            :param name: Whether or not this service should be subject to fault injection.
            :param fault_injection: Whether or not this service should be subject to fault injection.
            :param depends_on: Any services that this service depends on to successfully run.
            """
            if fault_injection:
                ip_suffix = self.next_available_fault_enabled_ip
                self.next_available_fault_enabled_ip += 1
            else:
                ip_suffix = self.next_available_fault_disabled_ip
                self.next_available_fault_disabled_ip += 1
            return {
                "container_name": name, "hostname": name,
                "image": f'{"workload" if name == "workload" else "mongo-binaries"}:{self.tag}',
                "volumes": [
                    f"./logs/{name}:/var/log/mongodb/",
                    "./scripts:/scripts/",
                    f"./data/{name}:/data/db",
                ], "command": f"/bin/bash /scripts/{name}.sh", "networks": {
                    "antithesis-net": {"ipv4_address": f"10.20.20.{ip_suffix}"}
                }, "depends_on": depends_on
            }

        docker_compose_yml = {
            "version": "3.0", "services": {
                "workload":
                    create_docker_compose_service(
                        "workload", fault_injection=False, depends_on=[
                            process.logger.external_sut_hostname
                            for process in self.suite_fixture.all_processes()
                        ])
            }, "networks": {
                "antithesis-net": {
                    "driver": "bridge", "ipam": {"config": [{"subnet": "10.20.20.0/24"}]}
                }
            }
        }
        print("Writing workload init script...")
        with open(os.path.join(build_context, "scripts", "workload.sh"), "w") as workload_init:
            workload_init.write("tail -f /dev/null\n")

        print("Writing resmoke run script for convenience...")
        with open(os.path.join(build_context, "scripts", "run_resmoke.sh"), "w") as run_resmoke:
            run_resmoke.write(f'{self.get_resmoke_run_command()} "$@"\n')

        print("Writing mongo{d,s} init scripts...")
        for process in self.suite_fixture.all_processes():
            # Add the `Process` as a service in the docker-compose.yml
            service_name = process.logger.external_sut_hostname
            docker_compose_yml["services"][service_name] = create_docker_compose_service(
                service_name, fault_injection=True, depends_on=[])

            # Write the `Process` args as an init script
            with open(os.path.join(build_context, "scripts", f"{service_name}.sh"), "w") as file:
                file.write(" ".join(map(quote, process.args)) + '\n')

        print("Writing `docker-compose.yml`...")
        with open(os.path.join(build_context, "docker-compose.yml"), "w") as docker_compose:
            docker_compose.write(yaml.dump(docker_compose_yml) + '\n')

        print("Writing Dockerfile...")
        with open(os.path.join(build_context, "Dockerfile"), "w") as dockerfile:
            dockerfile.write("FROM scratch\n")
            dockerfile.write("COPY docker-compose.yml /\n")
            dockerfile.write("ADD scripts /scripts\n")
            dockerfile.write("ADD logs /logs\n")
            dockerfile.write("ADD data /data\n")
            dockerfile.write("ADD debug /debug\n")

    def _initialize_docker_compose_build_context(self, build_context) -> None:
        """
        Remove the old docker compose build context and create a new one.

        :param build_context: Filepath where the configuration is going to be set up.
        """
        try:
            shutil.rmtree(build_context)
        except FileNotFoundError as _:
            # `shutil.rmtree` throws FileNotFoundError if the path DNE. In that case continue as normal.
            pass
        except Exception as exc:
            exception_text = f"""
            Could not remove directory due to old artifacts from a previous run.

            Please remove this directory and try again -- you may need to force remove:
            `{os.path.relpath(build_context)}`
            """
            raise RequiresForceRemove(exception_text) from exc

        for volume in ["scripts", "logs", "data", "debug"]:
            os.makedirs(os.path.join(build_context, volume))

    def build_config_image(self):
        """
        Build the config image containing the `docker-compose.yml` file, init scripts and volumes for the suite.

        :return: None
        """
        # Build out the directory structure and write the startup scripts for the config image
        print(f"Preparing antithesis config image build context for `{self.suite_name}`...")
        self._initialize_docker_compose_build_context(self.DOCKER_COMPOSE_BUILD_CONTEXT)
        self._add_docker_compose_configuration_to_build_context(self.DOCKER_COMPOSE_BUILD_CONTEXT)

        # Our official builds happen in Evergreen. Assert debug symbols are on system.
        # If this is running locally, this is for development purposes only and debug symbols are not required.
        if self.in_evergreen:
            assert os.path.exists(self.MONGODB_DEBUGSYMBOLS
                                  ), f"No debug symbols available at: {self.MONGODB_DEBUGSYMBOLS}"
            print("Running in Evergreen -- copying debug symbols to build context...")
            shutil.copy(self.MONGODB_DEBUGSYMBOLS,
                        os.path.join(self.DOCKER_COMPOSE_BUILD_CONTEXT, "debug"))

        print(f"Done setting up antithesis config image build context for `{self.suite_name}...")
        print("Building antithesis config image...")
        subprocess.run([
            "docker", "build", "-t", f"{self.suite_name}:{self.tag}", "-f",
            f"{self.DOCKER_COMPOSE_BUILD_CONTEXT}/Dockerfile", self.DOCKER_COMPOSE_BUILD_CONTEXT
        ], stdout=sys.stdout, stderr=sys.stderr, check=True)
        print("Done building antithesis config image.")

    def build_workload_image(self):
        """
        Build the workload image.

        :param tag: Tag to use for the image.
        :return: None.
        """

        print("Prepping `workload` image build context...")
        # Set up build context
        self._fetch_mongodb_binaries()
        self._copy_mongo_binary_to_build_context(self.WORKLOAD_BUILD_CONTEXT)
        self._clone_mongo_repo_to_build_context(self.WORKLOAD_BUILD_CONTEXT)
        self._add_libvoidstar_to_build_context(self.WORKLOAD_BUILD_CONTEXT)

        # Build docker image
        print("Building workload image...")
        subprocess.run([
            "docker", "build", "-t", f"workload:{self.tag}", "-f", self.WORKLOAD_DOCKERFILE,
            self.WORKLOAD_BUILD_CONTEXT
        ], stdout=sys.stdout, stderr=sys.stderr, check=True)
        print("Done building workload image.")

    def build_mongo_binaries_image(self):
        """
        Build the mongo-binaries image.

        :return: None.
        """
        # Set up build context
        print("Prepping `mongo binaries` image build context...")

        self._fetch_mongodb_binaries()
        self._copy_mongodb_binaries_to_build_context(self.MONGO_BINARIES_BUILD_CONTEXT)
        self._add_libvoidstar_to_build_context(self.MONGO_BINARIES_BUILD_CONTEXT)

        # Build docker image
        print("Building mongo binaries image...")
        subprocess.run([
            "docker", "build", "-t", f"mongo-binaries:{self.tag}", "-f",
            self.MONGO_BINARIES_DOCKERFILE, self.MONGO_BINARIES_BUILD_CONTEXT
        ], stdout=sys.stdout, stderr=sys.stderr, check=True)
        print("Done building mongo binaries image.")

    def _fetch_mongodb_binaries(self):
        """
        Get MongoDB binaries and verify existence/validity.

        In CI the binaries from the `master` branch should already exist from the compile task.
        In that case, we just want to fetch the `last-continuous` and `last-lts` binaries.

        :return: None.
        """
        mongodb_binaries_destination = os.path.join(self.MONGODB_BINARIES_RELATIVE_DIR, "bin")

        if not self.in_evergreen and os.path.exists(mongodb_binaries_destination):
            print(
                f"\n\tRunning Locally - Found existing MongoDB binaries at: {mongodb_binaries_destination}\n"
            )
        # If local, fetch the binaries.
        elif not self.in_evergreen:
            # Ensure that `db-contrib-tool` is installed locally
            db_contrib_tool_error = """
            Could not find `db-contrib-tool` installation locally.

            You can install `db-contrib-tool` with the following command:
            `pip install db-contrib-tool` OR `pipx install db-contrib-tool`

            More info on `db-contrib-tool` available here: `https://github.com/10gen/db-contrib-tool`
            """
            assert subprocess.run(["which",
                                   "db-contrib-tool"]).returncode == 0, db_contrib_tool_error

            # Use `db-contrib-tool` to get MongoDB binaries for this image
            print("Running Locally - Fetching All MongoDB binaries for image build...")
            subprocess.run([
                "db-contrib-tool",
                "setup-repro-env",
                "--variant",
                "ubuntu2204",
                "--linkDir",
                mongodb_binaries_destination,
                "--installLastContinuous",
                "--installLastLTS",
                "master",
            ], stdout=sys.stdout, stderr=sys.stderr, check=True)
        elif self.in_evergreen:
            print(
                "Running in Evergreen - Fetching `last-continuous` and `last-lts` MongoDB binaries for image build..."
            )
            subprocess.run([
                "db-contrib-tool",
                "setup-repro-env",
                "--variant",
                "ubuntu2204",
                "--linkDir",
                mongodb_binaries_destination,
                "--installLastContinuous",
                "--installLastLTS",
                "--evergreenConfig",
                "./.evergreen.yml",
            ], stdout=sys.stdout, stderr=sys.stderr, check=True)

        # Verify the binaries were downloaded successfully
        for required_binary in [self.MONGO_BINARY, self.MONGOD_BINARY, self.MONGOS_BINARY]:
            assert os.path.exists(
                required_binary
            ), f"Could not find Ubuntu 22.04 MongoDB binary at: {required_binary}"

            # Our official builds happen in Evergreen.
            # We want to ensure the binaries are linked with `libvoidstar.so` during image build.
            if self.in_evergreen:
                assert "libvoidstar" in subprocess.run(
                    ["ldd", required_binary],
                    check=True,
                    capture_output=True,
                ).stdout.decode(
                    "utf-8"), f"MongoDB binary is not linked to `libvoidstar.so`: {required_binary}"

    def _copy_mongo_binary_to_build_context(self, dir_path):
        """
        Copy the mongo binary to the build context.

        :param dir_path: Directory path to add mongo binary to.
        """
        mongo_binary_destination = os.path.join(dir_path, "mongo")

        print("Copy mongo binary to build context...")
        # Clean up any old artifacts in the build context
        if os.path.exists(mongo_binary_destination):
            print("Cleaning up mongo binary from build context first...")
            os.remove(mongo_binary_destination)
        shutil.copy(self.MONGO_BINARY, mongo_binary_destination)
        print("Done copying mongo binary to build context.")

    def _clone_mongo_repo_to_build_context(self, dir_path):
        """
        Clone the mongo repo to the build context.

        :param dir_path: Directory path to clone mongo repo to.
        """
        mongo_repo_destination = os.path.join(dir_path, "src")

        # Our official builds happen in Evergreen. Assert there's already a `mongo` repo in the build context.
        # This is because we cannot rely on the standard "git clone" command to include uncommitted changes applied from a `patch`.
        # Instead, we rely on Evergreen's `git.get_project` which will correctly clone the repo and apply changes from the `patch`.
        if self.in_evergreen:
            assert os.path.exists(
                mongo_repo_destination), f"No `mongo` repo available at: {mongo_repo_destination}"
            print("Running in Evergreen -- no need to clone `mongo` repo since it already exists.")
            return

        # Clean up any old artifacts in the build context.
        if os.path.exists(mongo_repo_destination):
            print("Cleaning up MongoDB repo from build context first...")
            shutil.rmtree(mongo_repo_destination)

        print("Cloning current MongoDB repo to build context...")
        clone_repo_warning_message = """
        - If you have uncommitted changes, they will not be included in the `workload` image.
        - If you would like to include these changes, commit your changes and try again.
        """
        print(clone_repo_warning_message)

        # Copy the mongo repo to the build context.
        # If this fails to clone, the `git` library will raise an exception.
        active_branch = git.Repo("./").active_branch.name
        git.Repo.clone_from("./", mongo_repo_destination, branch=active_branch)
        print("Done cloning MongoDB repo to build context.")

    def _copy_mongodb_binaries_to_build_context(self, dir_path):
        """
        Copy the MongodDB binaries to the build context.

        :param dir_path: Directory path to add MongoDB binaries to.
        """
        print("Copy all MongoDB binaries to build context...")
        mongodb_binaries_destination = os.path.join(dir_path, "dist-test")

        # Clean up any old artifacts in the build context
        if os.path.exists(mongodb_binaries_destination):
            print("Cleaning up all MongoDB binaries from build context first...")
            shutil.rmtree(mongodb_binaries_destination)
        shutil.copytree(self.MONGODB_BINARIES_RELATIVE_DIR, mongodb_binaries_destination)
        print("Done copying all MongoDB binaries to build context.")

    def _add_libvoidstar_to_build_context(self, dir_path):
        """
        Add the antithesis instrumentation library from the system to the build context.

        :param dir_path: Directory path to add `libvoidstar.so` to.
        """
        print("Add `livoidstar.so` to build context...")
        libvoidstar_destination = os.path.join(dir_path, "libvoidstar.so")

        # Clean up any old artifacts in the build context
        if os.path.exists(libvoidstar_destination):
            print("Cleaning up `libvoidstar.so` from build context first...")
            os.remove(libvoidstar_destination)

        # Our official builds happen in Evergreen. Assert a "real" `libvoidstar.so` is on system.
        if self.in_evergreen:
            assert os.path.exists(
                self.LIBVOIDSTAR_PATH), f"No `libvoidstar.so` available at: {self.LIBVOIDSTAR_PATH}"
            print("Running in Evergreen -- using system `libvoidstar.so`.")
            shutil.copy(self.LIBVOIDSTAR_PATH, libvoidstar_destination)
            print("Done copying `libvoidstar.so` from system to build context")

        else:
            disclaimer_message = """
            This is a stub `libvoidstar.so` file. It does not actually do anything. For local development, we
            don't expect developers to have `libvoidstar.so` available, but it is necessary for building the
            base images and is part of the base image Dockerfile(s).
            """
            with open(libvoidstar_destination, "w") as file:
                file.write(disclaimer_message)
            print(
                "Done writing stub `libvoidstar.so` to build context -- This is for development only."
            )
