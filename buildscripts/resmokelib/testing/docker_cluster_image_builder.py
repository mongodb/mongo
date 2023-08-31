import os
import shutil
import subprocess
import sys

import git

from buildscripts.resmokelib.testing.docker_cluster_config_writer import DockerClusterConfigWriter


class DockerComposeImageBuilder:
    """Build images needed to run a resmoke suite against a MongoDB Docker Container topology."""

    def __init__(self, tag, in_evergreen):
        """
        Constructs a `DockerComposeImageBuilder` which can build images locally and in CI.

        :param tag: The tag to use for these images.
        :param in_evergreen: Whether this is running in Evergreen or not.
        """
        self.tag = tag
        self.in_evergreen = in_evergreen

        # Build context constants
        self.WORKLOAD_BUILD_CONTEXT = "buildscripts/antithesis/base_images/workload"
        self.WORKLOAD_DOCKERFILE = f"{self.WORKLOAD_BUILD_CONTEXT}/Dockerfile"

        self.MONGO_BINARIES_BUILD_CONTEXT = "buildscripts/antithesis/base_images/mongo_binaries"
        self.MONGO_BINARIES_DOCKERFILE = f"{self.MONGO_BINARIES_BUILD_CONTEXT}/Dockerfile"

        # Artifact constants
        self.MONGODB_BINARIES_RELATIVE_DIR = "dist-test" if in_evergreen else "antithesis-dist-test"
        self.MONGO_BINARY = f"{self.MONGODB_BINARIES_RELATIVE_DIR}/bin/mongo"
        self.MONGOD_BINARY = f"{self.MONGODB_BINARIES_RELATIVE_DIR}/bin/mongod"
        self.MONGOS_BINARY = f"{self.MONGODB_BINARIES_RELATIVE_DIR}/bin/mongos"

        self.LIBVOIDSTAR_PATH = "/usr/lib/libvoidstar.so"
        self.MONGODB_DEBUGSYMBOLS = "mongo-debugsymbols.tgz"

    def build_base_images(self):
        """
        Build the base images needed for the docker configuration.

        :return: None.
        """
        self._fetch_mongodb_binaries()
        print("Building base images...")
        self._build_mongo_binaries_image()
        self._build_workload_image()
        print("Done building base images.")

    def build_config_image(self, antithesis_suite_name):
        """
        Build the antithesis config image containing the `docker-compose.yml` file and volumes for the suite.

        :param antithesis_suite_name: The antithesis suite to build a docker compose config image for.
        :param tag: Tag to use for the docker compose configuration and/or base images.
        """

        # Build out the directory structure and write the startup scripts for the config image at runtime
        print(f"Prepping antithesis config image build context for `{antithesis_suite_name}`...")
        config_image_writer = DockerClusterConfigWriter(antithesis_suite_name, self.tag)
        config_image_writer.generate_docker_sharded_cluster_config()

        # Our official builds happen in Evergreen. Assert debug symbols are on system.
        # If this is running locally, this is for development purposes only and debug symbols are not required.
        if self.in_evergreen:
            assert os.path.exists(self.MONGODB_DEBUGSYMBOLS
                                  ), f"No debug symbols available at: {self.MONGODB_DEBUGSYMBOLS}"
            print("Running in Evergreen -- copying debug symbols to build context...")
            shutil.copy(self.MONGODB_DEBUGSYMBOLS,
                        os.path.join(config_image_writer.build_context, "debug"))

        print(
            f"Done setting up antithesis config image build context for `{antithesis_suite_name}..."
        )
        print("Building antithesis config image...")
        subprocess.run([
            "docker", "build", "-t", f"{antithesis_suite_name}:{self.tag}", "-f",
            f"{config_image_writer.build_context}/Dockerfile", config_image_writer.build_context
        ], stdout=sys.stdout, stderr=sys.stderr, check=True)
        print("Done building antithesis config image.")

    def _build_workload_image(self):
        """
        Build the workload image.

        :param tag: Tag to use for the image.
        :return: None.
        """

        print("Prepping `workload` image build context...")
        # Set up build context
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

    def _build_mongo_binaries_image(self):
        """
        Build the mongo-binaries image.

        :return: None.
        """

        print("Prepping `mongo binaries` image build context...")
        # Set up build context
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
        Get MongoDB binaries -- if running locally -- and verify existence/validity.

        In CI the binaries should already exist. In that case, we want to verify binary existence and
        that they are linked with `libvoidstar`.

        :return: None.
        """
        mongodb_binaries_destination = os.path.join(self.MONGODB_BINARIES_RELATIVE_DIR, "bin")

        # If local, fetch the binaries.
        if not self.in_evergreen:
            # Clean up any old artifacts in the build context.
            if os.path.exists(mongodb_binaries_destination):
                print("Removing old MongoDB binaries...")
                shutil.rmtree(mongodb_binaries_destination)

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
            print("Fetching MongoDB binaries for image build...")
            subprocess.run([
                "db-contrib-tool", "setup-repro-env", "--variant", "ubuntu2204", "--linkDir",
                mongodb_binaries_destination, "master"
            ], stdout=sys.stdout, stderr=sys.stderr, check=True)

        # Verify the binaries were downloaded successfully
        for required_binary in [self.MONGO_BINARY, self.MONGOD_BINARY, self.MONGOS_BINARY]:
            assert os.path.exists(
                required_binary
            ), f"Could not find Ubuntu 18.04 MongoDB binary at: {required_binary}"

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
        git.Repo("./").clone(mongo_repo_destination)
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
