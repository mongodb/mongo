from abc import abstractmethod
import configparser
from datetime import datetime
import multiprocessing
import os
import socket
import sys
import traceback
from typing import Any, Dict, List, Optional
import distro
import git
from pydantic import BaseModel

# pylint: disable=bare-except

SCONS_ENV_FILE = "scons_env.env"
SCONS_SECTION_HEADER = "SCONS_ENV"


class BaseMetrics(BaseModel):
    """Base class for an metrics object."""

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
    def get_scons_build_info(
            cls,
            utc_starttime: datetime,
            env_vars: "SCons.Variables.Variables",
            env: "SCons.Script.SConscript.SConsEnvironment",
            parser: "SCons.Script.SConsOptions.SConsOptionParser",
            args: List[str],
    ):
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

        try:
            # Use SCons built-in method to save environment variables to a file
            env_vars.Save(SCONS_ENV_FILE, env)

            # Add a section header to the file so we can easily parse with ConfigParser
            with open(SCONS_ENV_FILE, 'r') as original:
                data = original.read()
            with open(SCONS_ENV_FILE, 'w') as modified:
                modified.write(f"[{SCONS_SECTION_HEADER}]\n" + data)

            # Parse file using config parser
            config = configparser.ConfigParser()
            config.read(SCONS_ENV_FILE)
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


class ExitInfo(BaseMetrics):
    """Class to store tooling exit information."""

    exit_code: Optional[int]
    exception: Optional[str]
    stacktrace: Optional[str]

    @classmethod
    def get_resmoke_exit_info(cls):
        """Get the current exit info."""
        exc = sys.exc_info()[1]
        return cls(
            exit_code=0 if not exc else exc.code if exc.__class__ == SystemExit else 1,
            exception=exc.__class__.__name__ if exc else None,
            stacktrace=traceback.format_exc() if exc else None,
        )

    @classmethod
    def get_scons_exit_info(cls, exit_code):
        """Get the current exit info using the given exit code."""
        return cls(
            exit_code=exit_code if isinstance(exit_code, int) else None,
            exception=None,
            stacktrace=None,
        )

    def is_malformed(self):
        """Return True if this object is missing an exit code."""
        return self.exit_code is None


class HostInfo(BaseMetrics):
    """Class to store host information."""

    host_os: str
    num_cores: int
    memory: Optional[float]

    @classmethod
    def get_host_info(cls):
        """Get the host info to the best of our ability."""
        try:
            memory = cls._get_memory()
        except:
            memory = None
        return cls(
            host_os=distro.name(pretty=True),
            num_cores=multiprocessing.cpu_count(),
            memory=memory,
        )

    @staticmethod
    def _get_memory():
        """Get total memory of the host system."""
        return os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') / (1024.**3)

    def is_malformed(self):
        """Confirm whether this instance has all expected fields."""
        return self.memory is None


class GitInfo(BaseMetrics):
    """Class to store git repo information."""

    filepath: str
    commit_hash: Optional[str]
    branch_name: Optional[str]
    repo_name: Optional[str]

    @classmethod
    def get_git_info(cls, filepath: str):
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

    def is_malformed(self):
        """Confirm whether this instance has all expected fields."""
        return None in [self.commit_hash, self.branch_name, self.repo_name]


MODULES_FILEPATH = 'src/mongo/db/modules'


def _get_modules_git_info():
    """Get git info for all modules."""
    module_git_info = []
    try:
        module_git_info = [
            GitInfo.get_git_info(os.path.join(MODULES_FILEPATH, module))
            for module in os.listdir(MODULES_FILEPATH)
            if os.path.isdir(os.path.join(MODULES_FILEPATH, module))
        ]
    except:
        pass
    return module_git_info


class ToolingMetrics(BaseMetrics):
    """Class to store tooling metrics."""

    source: str
    utc_starttime: datetime
    utc_endtime: datetime
    host_info: HostInfo
    git_info: GitInfo
    exit_info: ExitInfo
    build_info: Optional[BuildInfo]
    command: List[str]
    module_info: List[GitInfo]
    ip_address: Optional[str]

    @classmethod
    def get_resmoke_metrics(
            cls,
            utc_starttime: datetime,
    ):
        """Get resmoke metrics to the best of our ability."""
        try:
            ip_address = socket.gethostbyname(socket.gethostname())
        except:
            ip_address = None
        return cls(
            source='resmoke',
            utc_starttime=utc_starttime,
            utc_endtime=datetime.utcnow(),
            host_info=HostInfo.get_host_info(),
            git_info=GitInfo.get_git_info('.'),
            exit_info=ExitInfo.get_resmoke_exit_info(),
            build_info=None,
            module_info=_get_modules_git_info(),
            command=sys.argv,
            ip_address=ip_address,
        )

    @classmethod
    def get_scons_metrics(
            cls,
            utc_starttime: datetime,
            env_vars: "SCons.Variables.Variables",
            env: "SCons.Script.SConscript.SConsEnvironment",
            parser: "SCons.Script.SConsOptions.SConsOptionParser",
            args: List[str],
            exit_code: int,
    ):
        """Get scons metrics to the best of our ability."""
        try:
            ip_address = socket.gethostbyname(socket.gethostname())
        except:
            ip_address = None
        return cls(
            source='scons',
            utc_starttime=utc_starttime,
            utc_endtime=datetime.utcnow(),
            host_info=HostInfo.get_host_info(),
            git_info=GitInfo.get_git_info('.'),
            exit_info=ExitInfo.get_scons_exit_info(exit_code),
            build_info=BuildInfo.get_scons_build_info(utc_starttime, env_vars, env, parser, args),
            module_info=_get_modules_git_info(),
            command=sys.argv,
            ip_address=ip_address,
        )

    def is_malformed(self):
        """Confirm whether this instance has all expected fields."""
        sub_metrics = [self.build_info] if self.source == 'scons' else []
        sub_metrics += self.module_info + [self.git_info] + [self.host_info] + [self.exit_info]
        return self.ip_address is None or any(metrics.is_malformed() for metrics in sub_metrics)
