"""Run the hang analyzer on the remote powercycle instance."""

import os
import re

from buildscripts.resmokelib.powercycle import powercycle_constants
from buildscripts.resmokelib.powercycle.lib import PowercycleCommand
from buildscripts.resmokelib.powercycle.lib.remote_operations import SSHOperation


class RunHangAnalyzerOnRemoteInstance(PowercycleCommand):
    """Run the hang-analyzer on a remote instance."""

    COMMAND = "runHangAnalyzerOnRemoteInstance"

    def execute(self) -> None:
        """:return: None."""
        if "private_ip_address" not in self.expansions:
            return
        hang_analyzer_processes = "dbtest,java,mongo,mongod,mongos,python,_test" if "hang_analyzer_processes" not in self.expansions else self.expansions[
            "hang_analyzer_processes"]
        hang_analyzer_option = f"-o file -o stdout -p {hang_analyzer_processes}"
        hang_analyzer_dump_core = True if "hang_analyzer_dump_core" not in self.expansions else self.expansions[
            "hang_analyzer_dump_core"]
        if hang_analyzer_dump_core:
            hang_analyzer_option = f"-c {hang_analyzer_option}"

        core_ext = "core"
        if self.is_windows():
            core_ext = "mdmp"
        remote_dir = powercycle_constants.REMOTE_DIR
        files = self._call("ls")[1].split("\n")
        dbg_regex = re.compile(r"(\.debug$)|(\.dSYM$)|(\.pdb$)")
        debug_files = [f for f in files if dbg_regex.match(f)]
        file_param = []
        for debug_file in debug_files:
            file_param.append(debug_file)
        if file_param:
            self.remote_op.operation(SSHOperation.COPY_TO, file_param, remote_dir)

        # Activate virtualenv on remote host. The virtualenv bin_dir is different for Linux and
        # Windows.
        venv = powercycle_constants.VIRTUALENV_DIR
        cmds = f"activate=$(find {venv} -name 'activate')"
        cmds = f"{cmds}; . $activate"

        # In the 'cmds' variable we pass to remote host, use 'python' instead of '$python' since
        # we don't want to evaluate the local python variable, but instead pass the python string
        # so the remote host will use the right python when the virtualenv is sourced.
        cmds = f"{cmds}; cd {remote_dir}"
        cmds = f"{cmds}; python buildscripts/resmoke.py hang-analyzer {hang_analyzer_option}"
        self.remote_op.operation(SSHOperation.SHELL, cmds, None)

        file_param = []
        file_param.append(f"{remote_dir}/debugger*.*")
        file_param.append(f"{remote_dir}/*.{core_ext}")
        self.remote_op.operation(SSHOperation.COPY_FROM, file_param, None)
