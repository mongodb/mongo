import os

import psutil

from buildscripts.resmokelib import config


def test_analysis(logger, pids):
    """
    Write the pids out to a file and kill them instead of running analysis.
    This option will only be specified in resmoke selftests.
    """
    with open(os.path.join(config.DBPATH_PREFIX, "test_analysis.txt"), "w") as analysis_file:
        analysis_file.write("\n".join([str(pid) for pid in pids]))
        for pid in pids:
            try:
                proc = psutil.Process(pid)
                logger.info("Killing process pid %d", pid)
                proc.kill()
            except psutil.NoSuchProcess:
                # Process has already terminated.
                pass
