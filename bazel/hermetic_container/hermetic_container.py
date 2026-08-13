import getpass
import hashlib
import json
import logging
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import collections
import time


HERMETIC_CONTAINER_RC_FILE = ".hermetic_containerrc"
HERMETIC_CONTAINER_RUN_FILE = ".hermetic_container_run"
BAZEL_WORKSPACE_FILES = ("WORKSPACE", "WORKSPACE.bazel", "MODULE.bazel")

DEFAULT_INSTANCE_NAME = "hermetic_container"
DEFAULT_IMAGE_NAME = "hermetic_container"
DEFAULT_RUN_COMMAND = "/bin/bash"
DEFAULT_DOCKER_COMMAND = "docker"
DEFAULT_LOCAL_DOCKERFILE = "Dockerfile.hermetic_container"
DEFAULT_REMOTE_REPOSITORY = "hermetic_container"
DEFAULT_DIRECTORY = os.getcwd()
DEFAULT_COMMAND = "/usr/bin/bazel"
DEFAULT_VOLUMES = []
DEFAULT_PORTS = []
DEFAULT_ENV_VARS = []
DEFAULT_GPUS = []
DEFAULT_PLATFORM = ""
DEFAULT_SHM_SIZE = ""
DEFAULT_NETWORK = "hermetic_container"
DEFAULT_RUN_DEPS = []
DEFAULT_DOCKER_COMPOSE_FILE = ""
DEFAULT_DOCKER_COMPOSE_COMMAND = "docker-compose"
DEFAULT_DOCKER_COMPOSE_PROJECT_NAME = "hermetic_container"
DEFAULT_DOCKER_COMPOSE_SERVICES = ""
DEFAULT_USER = ""
DEFAULT_DOCKER_BUILD_ARGS = ""

DEFAULT_DELEGATED_VOLUME = True
DEFAULT_BAZEL_USER_OUTPUT_ROOT = os.path.expanduser("~/.cache/bazel/_bazel_%s" % getpass.getuser())
TEMP_BAZEL_OUTPUT_USER_ROOT = "/var/bazel/workspace/_bazel_%s" % getpass.getuser()
DEFAULT_BAZEL_USER_OUTPUT_PATHS = ["external", "action_cache", "execroot"]
DEFAULT_BAZEL_RC_FILE = ""
DEFAULT_DOCKER_RUN_PRIVILEGED = False
DEFAULT_DOCKER_MACHINE = None
DEFAULT_WORKSPACE_HEX = False
DEFAULT_VOLUME_SOURCE_MODE = ""
DEFAULT_WSL_DRIVE_MOUNT_PREFIX = "/mnt"
CONTAINER_NETWORK_RETRY_ATTEMPTS = 3
CONTAINER_NETWORK_RETRY_DELAY_SECONDS = 1

DOCKER_SPECIAL_NETWORK_NAMES = ["host", "bridge", "none"]

logger = logging.getLogger("hermetic_container")


class DockerInstance:
    """Manages communication and runs commands on associated docker container.

    A DockerInstance can build the image for the container if necessary, run it,
    set it up through configuration variables, and pass on commands to it.
    It streams the output directly and blocks until the command finishes.
    """

    def __init__(
        self,
        instance_name,
        image_name,
        run_command,
        docker_command,
        dockerfile,
        repository,
        directory,
        command,
        volumes,
        ports,
        env_vars,
        gpus,
        network,
        run_deps,
        docker_compose_file,
        docker_compose_command,
        docker_compose_project_name,
        docker_compose_services,
        bazel_user_output_root,
        bazel_rc_file,
        docker_run_privileged,
        docker_machine,
        hermetic_container_run_file,
        workspace_hex,
        delegated_volume,
        user,
        docker_build_args,
        shm_size,
        platform,
    ):
        real_directory = os.path.realpath(directory)
        self.workspace_hex_digest = ""
        self.bazel_output_base_digest = ""
        self.instance_name = instance_name
        self.image_name = image_name
        self.run_command = run_command
        self.docker_command = docker_command
        self.dockerfile = dockerfile
        self.repository = repository
        self.directory = directory
        self.command = command
        self.network = network
        self.docker_compose_file = docker_compose_file
        self.docker_compose_command = docker_compose_command
        self.docker_compose_project_name = docker_compose_project_name
        self.bazel_user_output_root = bazel_user_output_root
        self.bazel_output_base = ""
        self.bazel_rc_file = bazel_rc_file
        self.docker_run_privileged = docker_run_privileged
        self.docker_machine = docker_machine
        self.hermetic_container_run_file = hermetic_container_run_file
        self.delegated_volume_flag = ":delegated" if delegated_volume else ""
        self.user = user
        self.platform = platform
        self.docker_build_args = docker_build_args
        self.shm_size = shm_size
        self.volume_source_mode = os.environ.get(
            "HERMETIC_CONTAINER_VOLUME_SOURCE_MODE", DEFAULT_VOLUME_SOURCE_MODE
        ).lower()
        self.wsl_drive_mount_prefix = os.environ.get(
            "HERMETIC_CONTAINER_WSL_DRIVE_MOUNT_PREFIX", DEFAULT_WSL_DRIVE_MOUNT_PREFIX
        )
        self.remote_directory = self._get_remote_directory(real_directory, add_drive=True)

        if workspace_hex:
            self.workspace_hex_digest = hashlib.sha256(real_directory.encode("ascii")).hexdigest()
            self.instance_name = "%s_%s" % (self.instance_name, self.workspace_hex_digest)
            self.docker_compose_project_name = "%s%s" % (
                self.docker_compose_project_name,
                self.workspace_hex_digest,
            )
            if os.path.exists(self.dockerfile):
                self.image_name = "%s_%s" % (self.image_name, self.workspace_hex_digest)

        if self.user:
            # When --output_base is not passed, Bazel derives this directory from the
            # workspace path using MD5. This is separate from workspace_hex_digest, which
            # identifies the container and intentionally uses SHA-256.
            self.bazel_output_base_digest = hashlib.md5(  # nosemgrep: insecure-hash-algorithm-md5
                real_directory.encode("ascii"), usedforsecurity=False
            ).hexdigest()
        else:
            self.bazel_output_base_digest = self.workspace_hex_digest

        if self.docker_compose_file and not self._is_predefined_network():
            self.network = "%s_%s" % (self.docker_compose_project_name, network)

        self._add_volumes(volumes)
        self._add_ports(ports)
        self._add_env_vars(env_vars)
        self._add_gpus(gpus)
        self._add_run_deps(run_deps)
        self._add_compose_services(docker_compose_services)

    @classmethod
    def from_config(cls):
        config = cls._config_from_file()
        config.update(cls._config_from_environment())
        return DockerInstance(
            instance_name=config.get("HERMETIC_CONTAINER_INSTANCE_NAME", DEFAULT_INSTANCE_NAME),
            image_name=config.get("HERMETIC_CONTAINER_IMAGE_NAME", DEFAULT_IMAGE_NAME),
            run_command=config.get("HERMETIC_CONTAINER_RUN_COMMAND", DEFAULT_RUN_COMMAND),
            docker_command=config.get("HERMETIC_CONTAINER_DOCKER_COMMAND", DEFAULT_DOCKER_COMMAND),
            dockerfile=config.get("HERMETIC_CONTAINER_DOCKERFILE", DEFAULT_LOCAL_DOCKERFILE),
            repository=config.get("HERMETIC_CONTAINER_REPOSITORY", DEFAULT_REMOTE_REPOSITORY),
            directory=config.get("HERMETIC_CONTAINER_DIRECTORY", DEFAULT_DIRECTORY),
            command=config.get("HERMETIC_CONTAINER_COMMAND", DEFAULT_COMMAND),
            volumes=config.get("HERMETIC_CONTAINER_VOLUMES", DEFAULT_VOLUMES),
            ports=config.get("HERMETIC_CONTAINER_PORTS", DEFAULT_PORTS),
            env_vars=config.get("HERMETIC_CONTAINER_ENV_VARS", DEFAULT_ENV_VARS),
            gpus=config.get("HERMETIC_CONTAINER_GPUS", DEFAULT_GPUS),
            platform=config.get("HERMETIC_CONTAINER_PLATFORM", DEFAULT_PLATFORM),
            shm_size=config.get("HERMETIC_CONTAINER_SHM_SIZE", DEFAULT_SHM_SIZE),
            network=config.get("HERMETIC_CONTAINER_NETWORK", DEFAULT_NETWORK),
            run_deps=config.get("HERMETIC_CONTAINER_RUN_DEPS", DEFAULT_RUN_DEPS),
            docker_compose_file=config.get(
                "HERMETIC_CONTAINER_DOCKER_COMPOSE_FILE", DEFAULT_DOCKER_COMPOSE_FILE
            ),
            docker_compose_command=config.get(
                "HERMETIC_CONTAINER_DOCKER_COMPOSE_COMMAND", DEFAULT_DOCKER_COMPOSE_COMMAND
            ),
            docker_compose_project_name=config.get(
                "HERMETIC_CONTAINER_DOCKER_COMPOSE_PROJECT_NAME",
                DEFAULT_DOCKER_COMPOSE_PROJECT_NAME,
            ),
            docker_compose_services=config.get(
                "HERMETIC_CONTAINER_DOCKER_COMPOSE_SERVICES", DEFAULT_DOCKER_COMPOSE_SERVICES
            ),
            bazel_rc_file=config.get("HERMETIC_CONTAINER_BAZEL_RC_FILE", DEFAULT_BAZEL_RC_FILE),
            bazel_user_output_root=config.get(
                "HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT", DEFAULT_BAZEL_USER_OUTPUT_ROOT
            ),
            docker_run_privileged=config.get(
                "HERMETIC_CONTAINER_DOCKER_RUN_PRIVILEGED", DEFAULT_DOCKER_RUN_PRIVILEGED
            ),
            docker_machine=config.get("HERMETIC_CONTAINER_DOCKER_MACHINE", DEFAULT_DOCKER_MACHINE),
            hermetic_container_run_file=config.get(
                "HERMETIC_CONTAINER_RUN_FILE", HERMETIC_CONTAINER_RUN_FILE
            ),
            workspace_hex=config.get("HERMETIC_CONTAINER_WORKSPACE_HEX", DEFAULT_WORKSPACE_HEX),
            delegated_volume=config.get(
                "HERMETIC_CONTAINER_DELEGATED_VOLUME", "DEFAULT_DELEGATED_VOLUME"
            ),
            user=config.get("HERMETIC_CONTAINER_USER", DEFAULT_USER),
            docker_build_args=config.get(
                "HERMETIC_CONTAINER_DOCKER_BUILD_ARGS", DEFAULT_DOCKER_BUILD_ARGS
            ),
        )

    def send_command(self, args, bazel_rc_file=None):
        term_size = shutil.get_terminal_size()

        docker_exec_command = [
            *self._command_argv(self.docker_command, "HERMETIC_CONTAINER_DOCKER_COMMAND"),
            "exec",
            "-i",
            "-e",
            "COLUMNS=%s" % term_size.columns,
            "-e",
            "LINES=%s" % term_size.lines,
            "-e",
            "TERM=%s" % os.environ.get("TERM", ""),
        ]
        if sys.stdout.isatty():
            docker_exec_command.append("-t")
        if self.docker_run_privileged:
            docker_exec_command.append("--privileged")
        if self.user:
            docker_exec_command.append("--user=%s" % self.user)
        docker_exec_command.append(self.instance_name)

        if not self.user:
            output_args = (
                [
                    "--output_user_root=%s" % TEMP_BAZEL_OUTPUT_USER_ROOT,
                    "--output_base=%s" % self.bazel_output_base,
                ]
                if self.command and self.bazel_output_base
                else []
            )
        else:
            output_args = (
                ["--output_user_root=%s" % self.bazel_user_output_root]
                if self.command and self.bazel_user_output_root
                else []
            )

        command = [*docker_exec_command]
        if self.command:
            command.extend(self._command_argv(self.command, "HERMETIC_CONTAINER_COMMAND"))
        if self.bazel_rc_file and self.command:
            command.append("--bazelrc=%s" % self.bazel_rc_file)
        if bazel_rc_file and self.command:
            command.append("--bazelrc=%s" % bazel_rc_file)
        command.extend(output_args)
        command.extend(args)

        docker_environment = self._docker_machine_environment()
        result = subprocess.run(
            command,
            check=False,
            env=docker_environment,
        )

        if sys.platform == "win32":
            self._fix_win_symlink(
                docker_exec_command,
                env=docker_environment,
            )
        return result.returncode

    def _fix_win_symlink(self, docker_exec_command, env=None):
        p = pathlib.Path(self.directory)
        paths = [*p.glob("bazel-*"), *(p / ".tmp").glob("bazel-*")]
        for path in paths:
            relative_path = path.relative_to(p)
            command = [*docker_exec_command, "realpath", relative_path.as_posix()]
            try:
                output = subprocess.check_output(command, env=env).decode().strip()
            except subprocess.CalledProcessError:
                logger.info("INFO: Skipping fixing symlink, it already exists.")
            else:
                drive = pathlib.PureWindowsPath(path.drive)
                local_directory = drive.joinpath(pathlib.PurePosixPath(output))
                path.unlink()
                path.symlink_to(local_directory, target_is_directory=True)

    def start(self):
        """Starts the hermetic_container docker container."""
        rc = 0

        # Verify that the docker executable exists.
        if not self._docker_exists():
            logger.error("ERROR: Docker executable could not be found!")
            return 1

        # Build or pull the relevant hermetic_container image.
        if os.path.exists(self.dockerfile):
            rc = self._build()
        else:
            rc = self._pull()
            # If we have the image, don't stop everything just because we
            # couldn't pull.
            if rc and self._image_exists():
                rc = 0
        if rc:
            return rc

        # If given a docker-compose file, start the services needed to run.
        if self.docker_compose_file and self._docker_compose_exists():
            rc = self._start_compose_services()
        else:
            # If not through docker-compose, run the various dependencies as
            # necessary ourselves.

            # Setup the network if necessary.
            if not self._network_exists() and not self._is_predefined_network():
                logger.info("Creating network: '%s'" % self.network)
                rc = self._start_network()
            if rc:
                return rc

            # Setup run dependencies if necessary.
            rc = self._start_run_deps()
        if rc:
            return rc

        # Run the container itself.
        return self._run_container()

    def is_running(self):
        """Checks if the container is currently running."""
        command = self._with_docker_machine(
            "%s ps  --no-trunc --filter name=^%s$" % (self.docker_command, self.instance_name)
        )
        output = self._run_command(command)
        is_running = self._string_exists(self.instance_name, output)

        # If we have a directory, make sure the running container is mapped to
        # the same one (if not we need to create a new container mapped to the
        # correct folder).
        if self.directory and is_running:
            real_directory = os.path.realpath(self.directory)
            dir_string = "%s:%s" % (
                self._docker_volume_source(real_directory),
                self.remote_directory,
            )
            command = self._with_docker_machine(
                '%s inspect --format="%s" "%s"'
                % (self.docker_command, "{{json .HostConfig.Binds}}", self.instance_name)
            )
            output = self._run_command(command).strip()
            binds = json.loads(output)
            is_running = any(dir_string in b for b in binds)

        # If we have a network, make sure the running container is using the
        # correct network (if not we need to create a new container on the
        # correct network).
        # Note: with proper naming conventions this shouldn't happen much.
        if self.network and is_running:
            command = self._with_docker_machine(
                '%s inspect --format="%s" "%s"'
                % (self.docker_command, "{{.NetworkSettings.Networks}}", self.instance_name)
            )
            output = self._run_command(command).strip()
            is_running = self.network in output
        return is_running

    def _run_silent_command(self, command, ignore_output=False, retry=False):
        for attempt in range(CONTAINER_NETWORK_RETRY_ATTEMPTS if retry else 1):
            if ignore_output:
                rc = subprocess.call(
                    command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, shell=True
                )
            else:
                rc = subprocess.call(command, stdout=sys.stderr, shell=True)

            if rc == 0 or attempt == (CONTAINER_NETWORK_RETRY_ATTEMPTS - 1):
                return rc

            delay = CONTAINER_NETWORK_RETRY_DELAY_SECONDS * 2**attempt
            logger.info(
                "Container command failed with exit code %s; retrying in %ss: %s",
                rc,
                delay,
                command,
            )
            time.sleep(delay)

        raise AssertionError("container network retry loop exited unexpectedly")

    def _run_command(self, command):
        return subprocess.check_output(command, shell=True).decode()

    def _string_exists(self, string, output):
        regex = re.compile(r"\b(?=\w)%s\b(?!\w)" % re.escape(string))
        return any(regex.findall(output))

    def _image_exists(self):
        """Checks if the hermetic_container image exists in the local repository."""
        image = "%s/%s" % (self.repository, self.image_name)
        command = self._with_docker_machine("%s image ls %s" % (self.docker_command, image))
        output = self._run_command(command)
        exists = len(output.splitlines()) > 1
        return exists

    def _build(self):
        """Builds the hermetic_container image from the local dockerfile."""
        if not os.path.exists(self.dockerfile):
            raise RuntimeError("No Dockerfile to build the hermetic_container image from.")

        command = "%s build %s -t %s/%s -f %s %s" % (
            self.docker_command,
            self.docker_build_args,
            self.repository,
            self.image_name,
            self.dockerfile,
            self.directory,
        )
        command = self._with_docker_machine(command)
        return self._run_silent_command(command, retry=True)

    def _pull(self):
        """Pulls the relevant image from the dockerhub repository."""
        if not self.repository:
            raise RuntimeError("No repository to pull the hermetic_container image from.")

        command = "%s pull %s/%s" % (self.docker_command, self.repository, self.image_name)
        command = self._with_docker_machine(command)
        return self._run_silent_command(command, retry=True)

    def _is_predefined_network(self):
        """Checks if the network is one of the default existing docker network types"""
        return self.network in DOCKER_SPECIAL_NETWORK_NAMES

    def _network_exists(self):
        """Checks if the network we need to use exists."""
        command = self._with_docker_machine("%s network ls" % (self.docker_command))
        output = self._run_command(command)
        return self._string_exists(self.network, output)

    def _start_network(self):
        """Starts the docker network the container will use."""
        if not self.network:
            return 0

        command = "%s network create %s" % (self.docker_command, self.network)
        command = self._with_docker_machine(command)
        return self._run_silent_command(command)

    def _start_run_deps(self):
        """Starts the containers that are marked as runtime dependencies."""
        for run_dep_image, run_dep_name in self.run_deps:
            run_dep_instance = DockerInstance(
                instance_name=run_dep_name,
                image_name=run_dep_image,
                run_command=None,
                docker_command=None,
                dockerfile=None,
                repository=None,
                directory=None,
                command=None,
                volumes=None,
                ports=None,
                gpus=None,
                platform=None,
                shm_size=None,
                network=self.network,
                run_deps=None,
                docker_compose_file=None,
                docker_compose_command=None,
                docker_compose_project_name=None,
                docker_compose_services=None,
                bazel_rc_file=None,
                bazel_user_output_root=None,
                docker_run_privileged=self.docker_run_privileged,
                docker_machine=self.docker_machine,
                hermetic_container_run_file=None,
            )
            if not run_dep_instance.is_running():
                logger.info(
                    "Starting run dependency: '%s' (name: '%s')" % (run_dep_image, run_dep_name)
                )
                run_dep_instance._run_container()

    def _start_compose_services(self):
        """Starts the docker-compose services."""
        if not self.docker_compose_file:
            return 0

        pull_command = "COMPOSE_PROJECT_NAME=%s %s -f %s pull --ignore-pull-failures %s" % (
            self.docker_compose_project_name,
            self.docker_compose_command,
            self.docker_compose_file,
            self.docker_compose_services,
        )
        pull_command = self._with_docker_machine(pull_command)
        rc = self._run_silent_command(pull_command, retry=True)
        if rc:
            return rc

        build_command = "COMPOSE_PROJECT_NAME=%s %s -f %s build %s" % (
            self.docker_compose_project_name,
            self.docker_compose_command,
            self.docker_compose_file,
            self.docker_compose_services,
        )
        build_command = self._with_docker_machine(build_command)
        rc = self._run_silent_command(build_command, retry=True)
        if rc:
            return rc

        up_command = "COMPOSE_PROJECT_NAME=%s %s -f %s up --force-recreate -d %s" % (
            self.docker_compose_project_name,
            self.docker_compose_command,
            self.docker_compose_file,
            self.docker_compose_services,
        )
        return self._run_silent_command(self._with_docker_machine(up_command))

    def _run_container(self):
        """Runs the container itself."""
        logger.info("Starting docker container '%s'..." % self.instance_name)
        command = "%s stop %s" % (self.docker_command, self.instance_name)
        self._run_silent_command(self._with_docker_machine(command), ignore_output=True)
        command = "%s rm %s" % (self.docker_command, self.instance_name)
        self._run_silent_command(self._with_docker_machine(command), ignore_output=True)
        command = "%s run -id --name=%s %s %s %s %s %s %s %s %s %s %s %s%s %s" % (
            self.docker_command,
            self.instance_name,
            ("--platform=%s" % self.platform) if self.platform else "",
            "--privileged" if self.docker_run_privileged else "",
            ("--user=%s" % self.user if self.user else ""),
            ("-w %s" % self.remote_directory if self.remote_directory else ""),
            self.volumes,
            self.ports,
            self.env_vars,
            self.gpus,
            ("--shm-size=%s" % self.shm_size) if self.shm_size else "",
            ("--net=%s" % self.network) if self.network else "",
            ("%s/" % self.repository) if self.repository else "",
            self.image_name,
            self.run_command if self.run_command else "",
        )
        rc = self._run_silent_command(self._with_docker_machine(command))
        if rc:
            return rc

        # Touch the hermetic_container run file to change the timestamp.
        if self.hermetic_container_run_file:
            open(self.hermetic_container_run_file, "w").write(self.instance_name + "\n")
            logger.info("Done.")

        return rc

    def _add_volumes(self, volumes):
        """Add the given volumes to the run string, and the bazel volumes we need anyway."""
        # This can only be intentional in code, so ignore None volumes.
        self.volumes = ""
        if volumes is None:
            return

        # HERMETIC_CONTAINER_VOLUMES can be a python iterable or a comma-separated string.
        if isinstance(volumes, str):
            volumes = [v.strip() for v in volumes.split(",")]
        elif volumes and not isinstance(volumes, collections.abc.Iterable):
            raise RuntimeError(
                "HERMETIC_CONTAINER_VOLUMES must be comma-separated string "
                "or python iterable of strings"
            )

        # Find the real source and output directories.
        real_directory = os.path.realpath(self.directory)
        volumes += [
            "%s:%s" % (self._docker_volume_source(real_directory), self.remote_directory),
        ]

        # If the user hasn't explicitly set a HERMETIC_CONTAINER_BAZEL_USER_OUTPUT_ROOT for
        # bazel, set it from the output directory so that we get the build
        # results on the host.
        real_bazelout = os.path.realpath(os.path.join(self.directory, "bazel-out", ".."))

        if not self.bazel_user_output_root and "/_bazel" in real_bazelout:
            parts = real_bazelout.split("/_bazel")
            first_part = parts[0]
            second_part = "/_bazel" + parts[1].split("/")[0]
            self.bazel_user_output_root = first_part + second_part

        # Add the bazel user output directory if it exists, or the real bazelout
        # directory if it does.
        if self.bazel_user_output_root:
            bazel_output_base = os.path.realpath(
                os.path.join(self.bazel_user_output_root, self.bazel_output_base_digest)
            )
            self.bazel_output_base = self._get_remote_directory(bazel_output_base)

            user_output_paths = DEFAULT_BAZEL_USER_OUTPUT_PATHS + [os.path.basename(real_directory)]
            for user_output_path in user_output_paths:
                real_user_output_path = os.path.realpath(
                    os.path.join(bazel_output_base, user_output_path)
                )
                if not os.path.isdir(real_user_output_path):
                    os.makedirs(real_user_output_path)
                volumes += [
                    "%s:%s%s"
                    % (
                        self._docker_volume_source(real_user_output_path),
                        self._get_remote_directory(real_user_output_path),
                        self.delegated_volume_flag,
                    )
                ]
        elif real_bazelout:
            volumes += [
                "%s:%s%s"
                % (
                    self._docker_volume_source(real_bazelout),
                    real_bazelout,
                    self.delegated_volume_flag,
                )
            ]
            self.bazel_output_base = real_bazelout

        # Make sure the path exists on the host.
        if self.bazel_user_output_root and not os.path.isdir(self.bazel_user_output_root):
            os.makedirs(self.bazel_user_output_root)

        # Calculate the volumes string.
        volumes = [self._map_volume_source(volume) for volume in volumes]
        self.volumes = '-v "%s"' % '" -v "'.join(volumes)

    def _get_remote_directory(self, local_directory, add_drive=False):
        remote_directory = local_directory
        if sys.platform == "win32":
            win_path = os.path.splitdrive(local_directory)[1]
            if add_drive:
                drive = os.path.splitdrive(local_directory)[0].strip(":")
                win_path = "/%s/%s" % (drive, win_path)
            remote_directory = str(pathlib.PureWindowsPath(win_path).as_posix())
        return remote_directory

    def _docker_volume_source(self, local_directory):
        """Return a host path as seen by the Docker daemon.

        With Docker Desktop, the Windows Docker CLI and daemon understand Windows
        paths directly. With a Docker Engine running inside WSL, the daemon sees
        Windows drives through WSL's drvfs mount points instead.
        """
        if self.volume_source_mode != "wsl":
            return local_directory

        drive, drive_path = os.path.splitdrive(local_directory)
        if not drive:
            return local_directory

        drive_name = drive.rstrip(":").lower()
        posix_path = pathlib.PureWindowsPath(drive_path).as_posix().lstrip("/")
        return str(pathlib.PurePosixPath(self.wsl_drive_mount_prefix, drive_name, posix_path))

    def _map_volume_source(self, volume):
        if self.volume_source_mode != "wsl":
            return volume

        source, rest = self._split_volume_source(volume)
        if not rest:
            return source
        return "%s:%s" % (self._docker_volume_source(source), rest)

    def _split_volume_source(self, volume):
        drive_match = re.match(r"^([A-Za-z]:[\\/][^:]*):(.*)$", volume)
        if drive_match:
            return drive_match.group(1), drive_match.group(2)

        source, sep, rest = volume.partition(":")
        if not sep:
            return volume, ""
        return source, rest

    def _add_ports(self, ports):
        """Add the given ports to the run string."""
        # This can only be intentional in code, so disregard.
        self.ports = ""
        if not ports:
            return

        # HERMETIC_CONTAINER_PORTS can be a python iterable or a comma-separated string.
        if isinstance(ports, str):
            ports = [p.strip() for p in ports.split(",")]
        elif ports and not isinstance(ports, collections.abc.Iterable):
            raise RuntimeError(
                "HERMETIC_CONTAINER_PORTS must be comma-separated string "
                "or python iterable of strings"
            )

        # calculate the ports string
        self.ports = '-p "%s"' % '" -p "'.join(ports)

    def _add_gpus(self, gpus):
        """Add the given ports to the run string."""
        # This can only be intentional in code, so disregard.
        self.gpus = ""
        if not gpus:
            return

        # HERMETIC_CONTAINER_GPUS can be a python iterable or a comma-separated string.
        if isinstance(gpus, str):
            gpus = [g.strip() for g in gpus.split(",")]
        elif gpus and not isinstance(gpus, collections.Iterable):
            raise RuntimeError(
                "HERMETIC_CONTAINER_GPUS must be comma-separated string "
                "or python iterable of strings"
            )

        # calculate the gpus string
        self.gpus = "--gpus %s" % ",".join(gpus)

    def _add_env_vars(self, env_vars):
        """Add the given env vars to the run string."""
        # This can only be intentional in code, so disregard.
        self.env_vars = ""
        if not env_vars:
            return

        # HERMETIC_CONTAINER_ENV_VARS can be a python iterable or a comma-separated string.
        if isinstance(env_vars, str):
            env_vars = [p.strip() for p in env_vars.split(",")]
        elif env_vars and not isinstance(env_vars, collections.abc.Iterable):
            raise RuntimeError(
                "HERMETIC_CONTAINER_ENV_VARS must be comma-separated string "
                "or python iterable of strings"
            )

        # calculate the env string
        self.env_vars = '-e "%s"' % '" -e "'.join(env_vars)

    def _add_run_deps(self, run_deps):
        """Adds the necessary runtime container dependencies to launch."""
        # This can only be intentional in code, so disregard.
        self.run_deps = ""
        if not run_deps:
            return

        # HERMETIC_CONTAINER_RUN_DEPS can be a python iterable or a comma-separated string.
        if isinstance(run_deps, str):
            run_deps = [rd.strip() for rd in run_deps.split(",")]
        elif run_deps and not isinstance(run_deps, collections.abc.Iterable):
            raise RuntimeError(
                "HERMETIC_CONTAINER_RUN_DEPS must be comma-separated string "
                "or python iterable of strings"
            )

        def extract_image_and_instance(run_dep):
            if "::" in run_dep:
                return tuple(run_dep.split("::"))
            return (run_dep, self.network + "_" + run_dep.replace("/", "_").replace(":", "_"))

        self.run_deps = [extract_image_and_instance(rd) for rd in run_deps]

    def _add_compose_services(self, docker_compose_services):
        """Add the given services to the docker-compose up string."""
        # This can only be intentional in code, so ignore None services.
        self.docker_compose_services = ""
        if not docker_compose_services:
            return

        # HERMETIC_CONTAINER_DOCKER_COMPOSE_SERVICES can be a python iterable or a
        # comma-separated string.
        if isinstance(docker_compose_services, str):
            docker_compose_services = [s.strip() for s in docker_compose_services.split(",")]
        elif docker_compose_services and not isinstance(
            docker_compose_services, collections.abc.Iterable
        ):
            raise RuntimeError(
                "HERMETIC_CONTAINER_DOCKER_COMPOSE_SERVICES must be comma-separated string "
                "or python iterable of strings"
            )

        # Create the actual services string.
        self.docker_compose_services = " ".join(docker_compose_services)

    def _docker_exists(self):
        """Checks if the basic docker executable exists."""
        return self._command_exists(self.docker_command)

    def _docker_compose_exists(self):
        """Checks if the docker-compose executable exists."""
        return self._command_exists(self.docker_compose_command)

    def _command_exists(self, cmd):
        """Checks if a command exists on the system."""
        rc = shutil.which(cmd)
        return rc is not None

    @staticmethod
    def _command_argv(command, name):
        try:
            argv = shlex.split(command)
        except ValueError as exc:
            raise RuntimeError("Invalid %s: %s" % (name, exc)) from exc
        if not argv:
            raise RuntimeError("%s must not be empty" % name)
        return argv

    def _docker_machine_environment(self):
        """Return the Docker Machine environment without evaluating a shell string."""
        if self.docker_machine is None or not self._command_exists("docker-machine"):
            return None

        result = subprocess.run(
            ["docker-machine", "env", "--shell", "bash", self.docker_machine],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError("Failed to configure Docker Machine %s" % self.docker_machine)

        environment = os.environ.copy()
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line.startswith("export "):
                continue
            assignments = shlex.split(line.removeprefix("export "))
            if len(assignments) != 1 or "=" not in assignments[0]:
                raise RuntimeError("Invalid Docker Machine environment assignment: %s" % line)
            key, value = assignments[0].split("=", 1)
            if not key.startswith("DOCKER_"):
                raise RuntimeError("Unexpected Docker Machine environment variable: %s" % key)
            environment[key] = value
        return environment

    def _with_docker_machine(self, cmd):
        if self.docker_machine is None or not self._command_exists("docker-machine"):
            return cmd
        return "eval $(docker-machine env %s) && (%s)" % (self.docker_machine, cmd)

    @classmethod
    def _config_from_file(cls):
        """Creates a configuration from a .hermetic_containerrc file."""
        directory = cls._find_workspace_directory()
        local_hermetic_containerrc_path = os.path.join(directory, HERMETIC_CONTAINER_RC_FILE)
        hermetic_containerrc_path = os.environ.get(
            "HERMETIC_CONTAINER_RC_FILE", local_hermetic_containerrc_path
        )

        if not os.path.exists(hermetic_containerrc_path):
            return {
                "HERMETIC_CONTAINER_DIRECTORY": os.environ.get(
                    "HERMETIC_CONTAINER_DIRECTORY", directory
                )
            }

        config = {}
        with open(hermetic_containerrc_path, "r") as hermetic_containerrc:
            exec(hermetic_containerrc.read(), config)
        config["HERMETIC_CONTAINER_DIRECTORY"] = os.environ.get(
            "HERMETIC_CONTAINER_DIRECTORY", directory
        )
        return config

    @classmethod
    def _config_from_environment(cls):
        """Creates a configuration from environment variables."""
        return {
            name: value
            for (name, value) in os.environ.items()
            if name.startswith("HERMETIC_CONTAINER_")
        }

    @classmethod
    def _find_workspace_directory(cls):
        """Find the workspace directory.

        This is done by traversing the directory structure from the given hermetic_container
        directory until we find a Bazel workspace marker.
        """
        directory = os.path.realpath(
            os.environ.get("HERMETIC_CONTAINER_DIRECTORY", DEFAULT_DIRECTORY)
        )
        while directory and not cls._is_workspace_directory(directory):
            parent_directory = os.path.dirname(directory)
            if parent_directory == directory:
                break
            directory = parent_directory
        if not cls._is_workspace_directory(directory):
            raise FileNotFoundError("ERROR: No Bazel workspace file found!")
        else:
            return directory

    @staticmethod
    def _is_workspace_directory(directory):
        return any(
            os.path.exists(os.path.join(directory, workspace_file))
            for workspace_file in BAZEL_WORKSPACE_FILES
        )


def main():
    # Read the configuration either from .hermetic_containerrc or from the environment.
    di = DockerInstance.from_config()

    # If there is no .hermetic_container_run file, or it is too old, start the DockerInstance.
    if (
        not os.path.exists(di.hermetic_container_run_file)
        or not di.is_running()
        or (
            os.path.exists(di.dockerfile)
            and os.path.getctime(di.dockerfile) > os.path.getctime(di.hermetic_container_run_file)
        )
    ):
        rc = di.start()
        if rc:
            return rc

    # Forward the command line arguments to the container.
    return di.send_command(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
