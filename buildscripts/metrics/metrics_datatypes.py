from abc import abstractmethod
from datetime import datetime
import multiprocessing
import os
import socket
import sys
import traceback
from typing import List, Optional
import distro
import git
from pydantic import BaseModel

# pylint: disable=bare-except


class BaseMetrics(BaseModel):
    """Base class for an metrics object."""

    @abstractmethod
    def is_malformed(self) -> bool:
        """Confirm whether this instance has all expected fields."""
        raise NotImplementedError


class ExitInfo(BaseMetrics):
    """Class to store tooling exit information."""

    exit_code: int
    exception: Optional[str]
    stacktrace: Optional[str]

    @classmethod
    def get_exit_info(cls):
        """Get the current exit info."""
        exc = sys.exc_info()[1]
        return cls(
            exit_code=0 if not exc else exc.code if exc.__class__ == SystemExit else 1,
            exception=exc.__class__.__name__ if exc else None,
            stacktrace=traceback.format_exc() if exc else None,
        )

    def is_malformed(self):
        """Return True since this object cannot be malformed if created with classmethod."""
        return True


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
        return self.commit_hash is None or self.branch_name is None or self.repo_name is None


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

    utc_starttime: datetime
    utc_endtime: datetime
    host_info: HostInfo
    git_info: GitInfo
    exit_info: ExitInfo
    command: List[str]
    module_info: List[GitInfo]
    ip_address: Optional[str]

    @classmethod
    def get_tooling_metrics(cls, utc_starttime: datetime):
        """Get tooling metrics to the best of our ability."""
        try:
            ip_address = socket.gethostbyname(socket.gethostname())
        except:
            ip_address = None
        return cls(
            utc_starttime=utc_starttime,
            utc_endtime=datetime.utcnow(),
            host_info=HostInfo.get_host_info(),
            git_info=GitInfo.get_git_info('.'),
            exit_info=ExitInfo.get_exit_info(),
            module_info=_get_modules_git_info(),
            command=sys.argv,
            ip_address=ip_address,
        )

    def is_malformed(self):
        """Confirm whether this instance has all expected fields."""
        return self.ip_address is None or any(
            info_obj.is_malformed()
            for info_obj in self.module_info + [self.git_info] + [self.host_info])
