import dataclasses
from datetime import datetime
import multiprocessing
import os
import socket
import sys
import traceback
from typing import List, Optional
import distro
import git

# pylint: disable=bare-except


@dataclasses.dataclass
class ExitInfo:
    """Class to store tooling exit information."""

    exit_code: int
    exception: Optional[str] = None
    stacktrace: Optional[str] = None

    def __init__(self):
        """Get the current exit info to the best of our ability."""
        exc = sys.exc_info()[1]
        if not exc:
            self.exit_code = 0
            return
        self.exit_code = exc.code if exc.__class__ == SystemExit else 1
        self.exception = exc.__class__.__name__
        self.stacktrace = traceback.format_exc()


@dataclasses.dataclass
class HostInfo:
    """Class to store host information."""

    host_os: str
    num_cores: int
    memory: Optional[float] = None

    def __init__(self):
        """Get the host info to the best of our ability."""
        self.host_os = distro.name(pretty=True)
        self.num_cores = multiprocessing.cpu_count()
        try:
            self.memory = self._get_memory()
        except:
            pass

    @staticmethod
    def _get_memory():
        """Get total memory of the host system."""
        return os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') / (1024.**3)


@dataclasses.dataclass
class GitInfo:
    """Class to store git repo information."""

    filepath: str
    commit_hash: Optional[str] = None
    branch_name: Optional[str] = None
    repo_name: Optional[str] = None

    def __init__(self, filepath: str):
        """Get the git info for a repo to the best of our ability."""
        self.filepath = filepath
        try:
            self.commit_hash = git.Repo(filepath).head.commit.hexsha
        except:
            pass
        try:
            self.branch_name = git.Repo(filepath).active_branch.name
        except:
            pass
        try:
            self.repo_name = git.Repo(filepath).working_tree_dir.split("/")[-1]
        except:
            pass


MODULES_FILEPATH = 'src/mongo/db/modules'


def _get_modules_git_info():
    """Get git info for all modules."""
    module_git_info = []
    try:
        module_git_info = [
            GitInfo(os.path.join(MODULES_FILEPATH, module))
            for module in os.listdir(MODULES_FILEPATH)
            if os.path.isdir(os.path.join(MODULES_FILEPATH, module))
        ]
    except:
        pass
    return module_git_info


@dataclasses.dataclass
class ToolingMetrics:
    """Class to store tooling metrics."""

    utc_starttime: datetime
    utc_endtime: datetime
    host_info: HostInfo
    git_info: GitInfo
    exit_info: ExitInfo
    command: List[str]
    module_info: List[GitInfo]
    ip_address: Optional[str] = None

    def __init__(self, utc_starttime: datetime):
        """Get tooling metrics to the best of our ability."""
        self.utc_starttime = utc_starttime
        self.utc_endtime = datetime.utcnow()
        self.host_info = HostInfo()
        self.git_info = GitInfo('.')
        self.exit_info = ExitInfo()
        self.module_info = _get_modules_git_info()
        self.command = sys.argv
        try:
            self.ip_address = socket.gethostbyname(socket.gethostname())
        except:
            pass
