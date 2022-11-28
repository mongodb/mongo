from abc import abstractmethod
import configparser
from datetime import datetime
import multiprocessing
import os
import socket
import sys
from typing import Any, Dict, List, Optional
import distro
import git
from pydantic import BaseModel

from buildscripts.metrics.tooling_exit_hook import _ExitHook

# pylint: disable=bare-except

SCONS_ENV_FILE = "scons_env.env"
SCONS_SECTION_HEADER = "SCONS_ENV"


class BaseMetrics(BaseModel):
    """Base class for an metrics object."""

    @classmethod
    @abstractmethod
    def generate_metrics(cls, **kwargs):
        """Generate metrics."""
        raise NotImplementedError

    @abstractmethod
    def is_malformed(self) -> bool:
        """Confirm whether this instance has all expected fields."""
        raise NotImplementedError


class BuildInfo(BaseMetrics):
    """Class to store the Build environment, options & artifacts."""

    env: Optional[Dict[str, Any]]
    options: Optional[Dict[str, Any]]
    build_artifacts: Optional[List[str]]
    artifact_dir: Optional[str]

    @classmethod
    def generate_metrics(
            cls,
            utc_starttime: datetime,
            env_vars: "SCons.Variables.Variables",
            env: "SCons.Script.SConscript.SConsEnvironment",
            parser: "SCons.Script.SConsOptions.SConsOptionParser",
            args: List[str],
    ):  # pylint: disable=arguments-differ
        """Get SCons build info to the best of our ability."""
        artifact_dir = cls._get_scons_artifact_dir(env)
        return cls(
            env=cls._get_scons_env_vars_dict(env_vars, env),
            options=cls._get_scons_options_dict(parser, args),
            build_artifacts=cls._get_artifacts(utc_starttime, artifact_dir),
            artifact_dir=artifact_dir,
        )

    @staticmethod
    def _get_scons_env_vars_dict(
            env_vars: "SCons.Variables.Variables",
            env: "SCons.Script.SConscript.SConsEnvironment",
    ) -> Optional[Dict[str, Any]]:
        """Get the environment variables options that can be set by users."""

        artifact_dir = BuildInfo._get_scons_artifact_dir(env)
        artifact_dir = artifact_dir if artifact_dir else '.'
        scons_env_filepath = f'{artifact_dir}/{SCONS_ENV_FILE}'
        try:
            # Use SCons built-in method to save environment variables to a file
            env_vars.Save(scons_env_filepath, env)

            # Add a section header to the file so we can easily parse with ConfigParser
            with open(scons_env_filepath, 'r') as original:
                data = original.read()
            with open(scons_env_filepath, 'w') as modified:
                modified.write(f"[{SCONS_SECTION_HEADER}]\n" + data)

            # Parse file using config parser
            config = configparser.ConfigParser()
            config.read(scons_env_filepath)
            str_dict = dict(config[SCONS_SECTION_HEADER])
            return {key: eval(val) for key, val in str_dict.items()}  # pylint: disable=eval-used
        except:
            return None

    @staticmethod
    def _get_scons_options_dict(
            parser: "SCons.Script.SConsOptions.SConsOptionParser",
            args: List[str],
    ) -> Optional[Dict[str, Any]]:
        """Get the scons cli options set by users."""
        try:
            scons_options, _ = parser.parse_args(args)
            return vars(scons_options)
        except:
            return None

    @staticmethod
    def _get_scons_artifact_dir(env: "SCons.Script.SConscript.SConsEnvironment") -> Optional[str]:
        """Get the artifact dir for this build."""
        try:
            return env.Dir('$BUILD_DIR').get_abspath()
        except:
            return None

    @staticmethod
    def _get_artifacts(utc_starttime: datetime, artifact_dir: str) -> List[str]:
        """Search a directory recursively for all files created after the given timestamp."""
        try:
            start_timestamp = datetime.timestamp(utc_starttime)
            artifacts = []
            for root, _, files in os.walk(artifact_dir):
                for file in files:
                    filepath = os.path.join(root, file)
                    _, ext = os.path.splitext(filepath)
                    if ext in ['.a', '.so', ''] and os.path.getmtime(filepath) >= start_timestamp:
                        artifacts.append(filepath)
            return artifacts
        except:
            return None

    def is_malformed(self) -> bool:
        """Confirm whether this instance has all expected fields."""
        return None in [self.artifact_dir, self.env, self.options, self.build_artifacts]


class HostInfo(BaseMetrics):
    """Class to store host information."""

    ip_address: Optional[str]
    host_os: str
    num_cores: int
    memory: Optional[float]

    @classmethod
    def generate_metrics(cls):  # pylint: disable=arguments-differ
        """Get the host info to the best of our ability."""
        try:
            ip_address = socket.gethostbyname(socket.gethostname())
        except:
            ip_address = None
        try:
            memory = cls._get_memory()
        except:
            memory = None
        return cls(
            ip_address=ip_address,
            host_os=distro.name(pretty=True),
            num_cores=multiprocessing.cpu_count(),
            memory=memory,
        )

    @staticmethod
    def _get_memory():
        """Get total memory of the host system."""
        return os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') / (1024.**3)

    def is_malformed(self) -> bool:
        """Confirm whether this instance has all expected fields."""
        return None in [self.memory, self.ip_address]


class GitInfo(BaseMetrics):
    """Class to store git repo information."""

    filepath: str
    commit_hash: Optional[str]
    branch_name: Optional[str]
    repo_name: Optional[str]

    @classmethod
    def generate_metrics(cls, filepath: str):  # pylint: disable=arguments-differ
        """Get the git info for a repo to the best of our ability."""
        try:
            commit_hash = git.Repo(filepath).head.commit.hexsha
        except:
            commit_hash = None
        try:
            if git.Repo(filepath).head.is_detached:
                branch_name = commit_hash
            else:
                branch_name = git.Repo(filepath).active_branch.name
        except:
            branch_name = None
        try:
            repo_name = git.Repo(filepath).working_tree_dir.split("/")[-1]
        except:
            repo_name = None
        return cls(
            filepath=filepath,
            commit_hash=commit_hash,
            branch_name=branch_name,
            repo_name=repo_name,
        )

    def is_malformed(self) -> bool:
        """Confirm whether this instance has all expected fields."""
        return None in [self.commit_hash, self.branch_name, self.repo_name]


MODULES_FILEPATH = 'src/mongo/db/modules'


def _get_modules_git_info():
    """Get git info for all modules."""
    module_git_info = []
    try:
        module_git_info = [
            GitInfo.generate_metrics(os.path.join(MODULES_FILEPATH, module))
            for module in os.listdir(MODULES_FILEPATH)
            if os.path.isdir(os.path.join(MODULES_FILEPATH, module))
        ]
    except:
        pass
    return module_git_info


class ResmokeToolingMetrics(BaseMetrics):
    """Class to store resmoke tooling metrics."""

    source: str
    utc_starttime: datetime
    utc_endtime: datetime
    host_info: HostInfo
    git_info: GitInfo
    exit_code: Optional[int]
    command: List[str]
    module_info: List[GitInfo]

    @classmethod
    def generate_metrics(
            cls,
            utc_starttime: datetime,
            exit_hook: _ExitHook,
    ):  # pylint: disable=arguments-differ
        """Get resmoke metrics to the best of our ability."""
        return cls(
            source='resmoke',
            utc_starttime=utc_starttime,
            utc_endtime=datetime.utcnow(),
            host_info=HostInfo.generate_metrics(),
            git_info=GitInfo.generate_metrics('.'),
            exit_code=exit_hook.exit_code if isinstance(exit_hook.exit_code, int) else None,
            command=sys.argv,
            module_info=_get_modules_git_info(),
        )

    def is_malformed(self) -> bool:
        """Confirm whether this instance has all expected fields."""
        sub_metrics = self.module_info + [self.git_info] + [self.host_info]
        return self.exit_code is None or any(metrics.is_malformed() for metrics in sub_metrics)


class SConsToolingMetrics(BaseMetrics):
    """Class to store scons tooling metrics."""

    source: str
    utc_starttime: datetime
    utc_endtime: datetime
    host_info: HostInfo
    git_info: GitInfo
    exit_code: Optional[int]
    build_info: BuildInfo
    command: List[str]
    module_info: List[GitInfo]

    @classmethod
    def generate_metrics(
            cls,
            utc_starttime: datetime,
            env_vars: "SCons.Variables.Variables",
            env: "SCons.Script.SConscript.SConsEnvironment",
            parser: "SCons.Script.SConsOptions.SConsOptionParser",
            args: List[str],
            exit_hook: _ExitHook,
    ):  # pylint: disable=arguments-differ
        """Get scons metrics to the best of our ability."""
        return cls(
            source='scons',
            utc_starttime=utc_starttime,
            utc_endtime=datetime.utcnow(),
            host_info=HostInfo.generate_metrics(),
            git_info=GitInfo.generate_metrics('.'),
            build_info=BuildInfo.generate_metrics(utc_starttime, env_vars, env, parser, args),
            exit_code=exit_hook.exit_code if isinstance(exit_hook.exit_code, int) else None,
            command=sys.argv,
            module_info=_get_modules_git_info(),
        )

    def is_malformed(self) -> bool:
        """Confirm whether this instance has all expected fields."""
        sub_metrics = self.module_info + [self.git_info] + [self.host_info] + [self.build_info]
        return self.exit_code is None or any(metrics.is_malformed() for metrics in sub_metrics)
