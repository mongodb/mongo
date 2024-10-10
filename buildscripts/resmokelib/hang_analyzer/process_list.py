"""Functions to list processes in each OS and search for interesting processes."""

import csv
import io
import os
import sys
from typing import List, NamedTuple, Union

from buildscripts.resmokelib.hang_analyzer.process import call, callo, find_program


class Pinfo(NamedTuple):
    """Holds a vector of PIDs of the same process type."""

    name: str
    pidv: Union[int, List[int]]


def get_processes(process_ids, interesting_processes, process_match, logger):
    """
    Find all running interesting processes.

    If a list of process_ids is supplied, match on that.
    Otherwise, do a substring match on interesting_processes.

    :param process_ids: List of PIDs to match on.
    :param interesting_processes: List of process names to match on.
    :param process_match: 'exact' or 'contains'.
    :param logger: Where to log output.
    :param all_processes: List of all running (pid, process_name) pairs to search through.

    :return: A list Pinfo objects for matched processes.
    """
    ps = _get_lister()
    all_processes = ps.dump_processes(logger)

    # Canonicalize the process names to lowercase to handle cases where the name of the Python
    # process is /System/Library/.../Python on OS X and -p python is specified to the hang analyzer.
    all_processes = [
        Pinfo(name=process_name.lower(), pidv=pid) for (pid, process_name) in all_processes
    ]

    if process_ids:
        running_pids = {pidv for (pname, pidv) in all_processes}
        missing_pids = set(process_ids) - running_pids
        if missing_pids:
            logger.warning(
                "The following requested process ids are not running %s", list(missing_pids)
            )

    processes_to_keep = []
    for process in all_processes:
        # skip self
        if process.pidv == os.getpid():
            continue

        # if we've been given a list of pids, and the pid isn't in that list
        # skip it
        if process_ids and process.pidv not in process_ids:
            continue

        # if we don't have a list of pids, make sure the process matches
        # the list of interesting processes
        if (
            not process_ids
            and interesting_processes
            and not _pname_match(process_match, process.name, interesting_processes)
        ):
            continue

        processes_to_keep.append(process)

    process_types = {pname for (pname, _) in processes_to_keep}
    processes = [
        Pinfo(name=ptype, pidv=[pidv for (pname, pidv) in processes_to_keep if pname == ptype])
        for ptype in process_types
    ]

    logger.info("Found %d interesting processes %s", len(processes_to_keep), processes)

    return processes


def _get_lister():
    """Return _ProcessList object for OS."""
    if sys.platform.startswith("linux"):
        ps = _LinuxProcessList()
    elif sys.platform in ["win32", "cygwin"]:
        ps = _WindowsProcessList()
    elif sys.platform == "darwin":
        ps = _DarwinProcessList()
    else:
        raise OSError("Hang analyzer: Unsupported platform: {}".format(sys.platform))

    return ps


class _ProcessList(object):
    """Abstract base class for all process listers."""

    def dump_processes(self, logger):
        """
        Find all processes.

        :param logger: Where to log output.
        :return: A list of process names.
        """
        raise NotImplementedError("dump_process must be implemented in OS-specific subclasses")


class _WindowsProcessList(_ProcessList):
    """_WindowsProcessList class."""

    @staticmethod
    def __find_ps():
        """Find tasklist."""
        return os.path.join(os.environ["WINDIR"], "system32", "tasklist.exe")

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]."""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s", ps)

        ret = callo([ps, "/FO", "CSV"], logger)

        buff = io.StringIO(ret)
        csv_reader = csv.reader(buff)

        return [[int(row[1]), row[0]] for row in csv_reader if row[1] != "PID"]


class _DarwinProcessList(_ProcessList):
    """_DarwinProcessList class."""

    @staticmethod
    def __find_ps():
        """Find ps."""
        return find_program("ps", ["/bin"])

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]."""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s", ps)

        ret = callo([ps, "-axco", "pid,comm"], logger)

        buff = io.StringIO(ret)
        csv_reader = csv.reader(buff, delimiter=" ", quoting=csv.QUOTE_NONE, skipinitialspace=True)

        return [[int(row[0]), row[1]] for row in csv_reader if row[0] != "PID"]


class _LinuxProcessList(_ProcessList):
    """_LinuxProcessList class."""

    @staticmethod
    def __find_ps():
        """Find ps."""
        return find_program("ps", ["/bin", "/usr/bin"])

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]."""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s", ps)

        call([ps, "--version"], logger)

        ret = callo([ps, "-eo", "pid,args"], logger)

        buff = io.StringIO(ret)
        csv_reader = csv.reader(buff, delimiter=" ", quoting=csv.QUOTE_NONE, skipinitialspace=True)

        return [[int(row[0]), os.path.split(row[1])[1]] for row in csv_reader if row[0] != "PID"]


def _pname_match(match_type, pname, interesting_processes):
    """Return True if the pname matches an interesting_processes."""
    pname = os.path.splitext(pname)[0]
    for ip in interesting_processes:
        if match_type == "exact" and pname == ip or match_type == "contains" and ip in pname:
            return True
    return False
