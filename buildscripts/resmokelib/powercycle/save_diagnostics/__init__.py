"""Save various diagnostics info from the remote powercycle instance."""

from buildscripts.resmokelib.powercycle import powercycle_constants
from buildscripts.resmokelib.powercycle.lib import PowercycleCommand
from buildscripts.resmokelib.powercycle.lib.remote_operations import SSHOperation


class TarEC2Artifacts(PowercycleCommand):
    """Tar EC2 artifacts."""

    COMMAND = "tarEC2Artifacts"

    def execute(self) -> None:
        """:return: None."""
        if "ec2_ssh_failure" in self.expansions:
            return
        tar_cmd = "tar" if "tar" not in self.expansions else self.expansions["tar"]
        ec2_artifacts = powercycle_constants.LOG_PATH
        # On test success, we only archive mongod.log.
        if self.expansions.get("exit_code", "1") != "0":
            ec2_artifacts = f"{ec2_artifacts} {powercycle_constants.DB_PATH}"
            ec2_artifacts = f"{ec2_artifacts} {powercycle_constants.BACKUP_ARTIFACTS}"
            if self.is_windows():
                ec2_artifacts = f"{ec2_artifacts} {powercycle_constants.EVENT_LOGPATH}"

        cmd = f"{tar_cmd} czf ec2_artifacts.tgz {ec2_artifacts}"

        self.remote_op.operation(SSHOperation.SHELL, cmd, None)


class CopyEC2Artifacts(PowercycleCommand):
    """Copy EC2 artifacts."""

    COMMAND = "copyEC2Artifacts"

    def execute(self) -> None:
        """:return: None."""
        if "ec2_ssh_failure" in self.expansions:
            return

        self.remote_op.operation(SSHOperation.COPY_FROM, "ec2_artifacts.tgz", None)


class GatherRemoteEventLogs(PowercycleCommand):
    """
    Gather remote event logs.

    The event logs on Windows are a useful diagnostic to have when determining if something bad
    happened to the remote machine after it was repeatedly crashed during powercycle testing. For
    example, the Application and System event logs have previously revealed that the mongod.exe
    process abruptly exited due to not being able to open a file despite the process successfully
    being restarted and responding to network requests.
    """

    COMMAND = "gatherRemoteEventLogs"

    def execute(self) -> None:
        """:return: None."""
        if not self.is_windows() or self.expansions.get("ec2_ssh_failure", ""):
            return

        event_logpath = powercycle_constants.EVENT_LOGPATH
        cmds = f"mkdir -p {event_logpath}"
        cmds = f"{cmds}; wevtutil qe Application /c:10000 /rd:true /f:Text > {event_logpath}/application.log"
        cmds = f"{cmds}; wevtutil qe Security    /c:10000 /rd:true /f:Text > {event_logpath}/security.log"
        cmds = f"{cmds}; wevtutil qe System      /c:10000 /rd:true /f:Text > {event_logpath}/system.log"

        self.remote_op.operation(SSHOperation.SHELL, cmds, None)


class GatherRemoteMongoCoredumps(PowercycleCommand):
    """Gather Remote Mongo Coredumps."""

    COMMAND = "gatherRemoteMongoCoredumps"

    def execute(self) -> None:
        """:return: None."""
        if "ec2_ssh_failure" in self.expansions:
            return

        remote_dir = powercycle_constants.REMOTE_DIR
        # Find all core files and move to $remote_dir
        cmds = "core_files=$(/usr/bin/find -H . \\( -name '*.core' -o -name '*.mdmp' \\) 2> /dev/null)"
        cmds = f"{cmds}; if [ -z \"$core_files\" ]; then exit 0; fi"
        cmds = f"{cmds}; echo Found remote core files $core_files, moving to $(pwd)"
        cmds = f"{cmds}; for core_file in $core_files"
        cmds = f"{cmds}; do base_name=$(echo $core_file | sed 's/.*///')"
        cmds = f"{cmds};   if [ ! -f $base_name ]; then mv $core_file .; fi"
        cmds = f"{cmds}; done"

        self.remote_op.operation(SSHOperation.SHELL, cmds, remote_dir)


class CopyRemoteMongoCoredumps(PowercycleCommand):
    """Copy Remote Mongo Coredumps."""

    COMMAND = "copyRemoteMongoCoredumps"

    def execute(self) -> None:
        """:return: None."""
        if self.expansions.get("ec2_ssh_failure", ""):
            return

        if self.is_windows():
            core_suffix = "mdmp"
        else:
            core_suffix = "core"

        remote_dir = powercycle_constants.REMOTE_DIR
        # Core file may not exist so we ignore the return code.
        self.remote_op.operation(SSHOperation.SHELL, f"{remote_dir}/*.{core_suffix}", None, True)


class CopyEC2MonitorFiles(PowercycleCommand):
    """Copy EC2 monitor files."""

    COMMAND = "copyEC2MonitorFiles"

    def execute(self) -> None:
        """:return: None."""
        tar_cmd = "tar" if "tar" not in self.expansions else self.expansions["tar"]
        cmd = f"{tar_cmd} czf ec2_monitor_files.tgz {powercycle_constants.EC2_MONITOR_FILES}"

        self.remote_op.operation(SSHOperation.SHELL, cmd, None)
        self.remote_op.operation(SSHOperation.COPY_FROM, 'ec2_monitor_files.tgz', None)
