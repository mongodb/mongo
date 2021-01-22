"""Set up powercycle remote operations."""
import getpass
import os
import re
import subprocess
import sys

import shlex
import yaml

from buildscripts.powercycle_setup.remote_operations import RemoteOperations, SSHOperation
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.powercycle import powercycle_constants


class PowercycleCommand(Subcommand):  # pylint: disable=abstract-method, too-many-instance-attributes
    """Base class for remote operations to set up powercycle."""

    def __init__(self):
        """Initialize PowercycleCommand."""

        self.expansions = yaml.safe_load(open(powercycle_constants.EXPANSIONS_FILE))

        self.exe = ".exe" if self.is_windows() else ""
        self.retries = 0 if "ssh_retries" not in self.expansions else int(
            self.expansions["ssh_retries"])
        self.ssh_identity = self._get_ssh_identity()
        self.ssh_connection_options = self.ssh_identity + " " + self.expansions[
            "ssh_connection_options"]
        self.sudo = "" if self.is_windows() else "sudo"
        # The username on the Windows image that powercycle uses is currently the default user.
        self.user = "Administrator" if self.is_windows() else getpass.getuser()
        self.user_host = self.user + "@" + self.expansions["private_ip_address"]

        self.remote_op = RemoteOperations(
            user_host=self.user_host,
            ssh_connection_options=self.ssh_connection_options,
        )

    @staticmethod
    def is_windows() -> bool:
        """:return: True if running on Windows."""
        return sys.platform == "win32" or sys.platform == "cygwin"

    @staticmethod
    def _call(cmd):
        cmd = shlex.split(cmd)
        # Use a common pipe for stdout & stderr for logging.
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        buff_stdout, _ = process.communicate()
        buff = buff_stdout.decode("utf-8", "replace")
        return process.poll(), buff

    def _get_posix_workdir(self) -> str:
        workdir = self.expansions['workdir']
        if self.is_windows():
            workdir = workdir.replace("\\", "/")
        return workdir

    def _get_ssh_identity(self) -> str:
        workdir = self._get_posix_workdir()
        pem_file = '/'.join([workdir, 'powercycle.pem'])

        return f"-i {pem_file}"


class SetUpEC2Instance(PowercycleCommand):
    """Set up EC2 instance."""

    COMMAND = "setUpEC2Instance"

    def execute(self) -> None:  # pylint: disable=too-many-instance-attributes, too-many-locals, too-many-statements
        """:return: None."""

        # First operation -
        # Create remote_dir.
        group_cmd = f"id -Gn {self.user}"
        _, group = self._call(group_cmd)
        group = group.split(" ")[0]
        user_group = f"{self.user}:{group}"

        remote_dir = powercycle_constants.REMOTE_DIR
        db_path = powercycle_constants.DB_PATH

        set_permission_stmt = f"chmod -R 777"
        if self.is_windows():
            set_permission_stmt = f"setfacl -s user::rwx,group::rwx,other::rwx"
        cmds = f"{self.sudo} mkdir -p {remote_dir}; {self.sudo} chown -R {user_group} {remote_dir}; {set_permission_stmt} {remote_dir}; ls -ld {remote_dir}"
        cmds = f"{cmds}; {self.sudo} mkdir -p {db_path}; {self.sudo} chown -R {user_group} {db_path}; {set_permission_stmt} {db_path}; ls -ld {db_path}"

        self.remote_op.operation(SSHOperation.SHELL, cmds, None)

        # Second operation -
        # Copy buildscripts and mongoDB executables to the remote host.
        files = ['etc', 'buildscripts']
        mongo_executables = ["mongo", "mongod", "mongos"]
        for executable in mongo_executables:
            files.append(f"dist-test/bin/{executable}{self.exe}")

        self.remote_op.operation(SSHOperation.COPY_TO, files, remote_dir)

        # Third operation -
        # Set up virtualenv on remote.
        venv = powercycle_constants.VIRTUALENV_DIR
        python = "/opt/mongodbtoolchain/v3/bin/python3" if "python" not in self.expansions else self.expansions[
            "python"]

        cmds = f"python_loc=$(which {python})"
        cmds = f"{cmds}; remote_dir={remote_dir}"
        cmds = f"{cmds}; if [ 'Windows_NT' = '$OS' ]; then python_loc=$(cygpath -w $python_loc); remote_dir=$(cygpath -w $remote_dir); fi"
        cmds = f"{cmds}; virtualenv --python $python_loc --system-site-packages {venv}"
        cmds = f"{cmds}; activate=$(find {venv} -name 'activate')"
        cmds = f"{cmds}; . $activate"
        cmds = f"{cmds}; pip3 install -r $remote_dir/etc/pip/powercycle-requirements.txt"

        self.remote_op.operation(SSHOperation.SHELL, cmds, None)

        # Fourth operation -
        # Enable core dumps on non-Windows remote hosts.
        # The core pattern must specify a director, since mongod --fork will chdir("/")
        # and cannot generate a core dump there (see SERVER-21635).
        # We need to reboot the host for the core limits to take effect.
        if not self.is_windows():
            core_pattern = f"{remote_dir}/dump_%e.%p.core"
            sysctl_conf = "/etc/sysctl.conf"
            cmds = "ulimit -a"
            cmds = f"{cmds}; echo \"{self.user} - core unlimited\" | {self.sudo} tee -a /etc/security/limits.conf"
            cmds = f"{cmds}; if [ -f {sysctl_conf} ]"
            cmds = f"{cmds}; then grep ^kernel.core_pattern {sysctl_conf}"
            cmds = f"{cmds};    if [ $? -eq  0 ]"
            cmds = f"{cmds};    then {self.sudo} sed -i \"s,kernel.core_pattern=.*,kernel.core_pattern=$core_pattern,\" {sysctl_conf}"
            cmds = f"{cmds};    else echo \"kernel.core_pattern={core_pattern}\" | {self.sudo} tee -a {sysctl_conf}"
            cmds = f"{cmds};    fi"
            cmds = f"{cmds}; else echo Cannot change the core pattern and no core dumps will be generated."
            cmds = f"{cmds}; fi"
            # The following line for restarting the machine is based on
            # https://unix.stackexchange.com/a/349558 in order to ensure the ssh client gets a
            # response from the remote machine before it restarts.
            cmds = f"{cmds}; nohup {self.sudo} reboot &>/dev/null & exit"
            self.remote_op.operation(SSHOperation.SHELL, cmds, None)

        # Fifth operation -
        # Print the ulimit & kernel.core_pattern
        if not self.is_windows():
            # Always exit successfully, as this is just informational.
            cmds = "uptime"
            cmds = f"{cmds}; ulimit -a"
            cmds = f"{cmds}; if [ -f /sbin/sysctl ]"
            cmds = f"{cmds}; then /sbin/sysctl kernel.core_pattern"
            cmds = f"{cmds}; fi"

            self.remote_op.operation(SSHOperation.SHELL, cmds, None, True)

        # Sixth operation -
        # Set up curator to collect system & process stats on remote.
        variant = "windows" if self.is_windows() else "ubuntu1604"
        curator_hash = "b0c3c0fc68bce26d9572796d6bed3af4a298e30e"
        curator_url = f"https://s3.amazonaws.com/boxes.10gen.com/build/curator/curator-dist-{variant}-{curator_hash}.tar.gz"
        cmds = f"curl -s {curator_url} | tar -xzv"
        monitor_system_file = powercycle_constants.MONITOR_SYSTEM_FILE
        monitor_proc_file = powercycle_constants.MONITOR_PROC_FILE
        if self.is_windows():
            # Since curator runs as SYSTEM user, ensure the output files can be accessed.
            cmds = f"{cmds}; touch {monitor_system_file}; chmod 777 {monitor_system_file}"
            cmds = f"{cmds}; cygrunsrv --install curator_sys --path curator --chdir $HOME --args 'stat system --file {monitor_system_file}'"
            cmds = f"{cmds}; touch {monitor_proc_file}; chmod 777 {monitor_proc_file}"
            cmds = f"{cmds}; cygrunsrv --install curator_proc --path curator --chdir $HOME --args 'stat process-all --file {monitor_proc_file}'"
            cmds = f"{cmds}; cygrunsrv --start curator_sys"
            cmds = f"{cmds}; cygrunsrv --start curator_proc"
        else:
            cmds = f"{cmds}; touch {monitor_system_file} {monitor_proc_file}"
            cmds = f"{cmds}; cmd=\"@reboot cd $HOME && {self.sudo} ./curator stat system >> {monitor_system_file}\""
            cmds = f"{cmds}; (crontab -l ; echo \"$cmd\") | crontab -"
            cmds = f"{cmds}; cmd=\"@reboot cd $HOME && $sudo ./curator stat process-all >> {monitor_proc_file}\""
            cmds = f"{cmds}; (crontab -l ; echo \"$cmd\") | crontab -"
            cmds = f"{cmds}; crontab -l"
            cmds = f"{cmds}; {{ {self.sudo} $HOME/curator stat system --file {monitor_system_file} > /dev/null 2>&1 & {self.sudo} $HOME/curator stat process-all --file {monitor_proc_file} > /dev/null 2>&1 & }} & disown"

        self.remote_op.operation(SSHOperation.SHELL, cmds, retry=True)

        # Seventh operation -
        # Install NotMyFault, used to crash Windows.
        if self.is_windows():
            windows_crash_zip = powercycle_constants.WINDOWS_CRASH_ZIP
            windows_crash_dl = powercycle_constants.WINDOWS_CRASH_DL
            windows_crash_dir = powercycle_constants.WINDOWS_CRASH_DIR

            cmds = f"curl -s -o {windows_crash_zip} {windows_crash_dl}"
            cmds = f"{cmds}; unzip -q {windows_crash_zip} -d {windows_crash_dir}"
            cmds = f"{cmds}; chmod +x {windows_crash_dir}/*.exe"
            self.remote_op.operation(SSHOperation.SHELL, cmds, None)


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


class RunHangAnalyzerOnRemoteInstance(PowercycleCommand):
    """Run the hang-analyzer on a remote instance."""

    COMMAND = "runHangAnalyzerOnRemoteInstance"

    def execute(self) -> None:  # pylint: disable=too-many-locals
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
        virtual_env = os.environ["VIRTUAL_ENV"]
        _, activate_loc = self._call(f"find {virtual_env} -name activate")
        bin_dir_regex = re.compile(f"{re.escape(virtual_env)}/(.*)/activate")
        bin_dir = bin_dir_regex.match(activate_loc)[1]
        cmds = f". {venv}/{bin_dir}/activate"
        # In the 'cmds' variable we pass to remote host, use 'python' instead of '$python' since
        # we don't want to evaluate the local python variable, but instead pass the python string
        # so the remote host will use the right python when the virtualenv is sourced.
        cmds = f"{cmds}; cd {remote_dir}"
        cmds = f"{cmds}; PATH=\"/opt/mongodbtoolchain/gdb/bin:$PATH\" python buildscripts/resmoke.py hang-analyzer {hang_analyzer_option}"
        self.remote_op.operation(SSHOperation.SHELL, cmds, None)

        file_param = []
        file_param.append(f"{remote_dir}/debugger*.*")
        file_param.append(f"{remote_dir}/*.{core_ext}")
        self.remote_op.operation(SSHOperation.COPY_FROM, file_param, None)


class NoOp(Subcommand):
    """No op."""

    def execute(self) -> None:
        """:return: None."""
        pass


class PowercyclePlugin(PluginInterface):
    """Interact with powercycle_operations."""

    def add_subcommand(self, subparsers):
        """
        Add 'powercycle_operations' subcommand.

        :param subparsers: argparse parser to add to
        :return: None
        """
        subparsers.add_parser(SetUpEC2Instance.COMMAND)
        subparsers.add_parser(TarEC2Artifacts.COMMAND)
        subparsers.add_parser(CopyEC2Artifacts.COMMAND)
        subparsers.add_parser(GatherRemoteEventLogs.COMMAND)
        subparsers.add_parser(GatherRemoteMongoCoredumps.COMMAND)
        subparsers.add_parser(CopyRemoteMongoCoredumps.COMMAND)
        subparsers.add_parser(CopyEC2MonitorFiles.COMMAND)
        subparsers.add_parser(RunHangAnalyzerOnRemoteInstance.COMMAND)
        # Accept arbitrary args like 'powercycle.py undodb foobar', but ignore them.

    def parse(self, subcommand, parser, parsed_args, **kwargs):  # pylint: disable=too-many-return-statements
        """
        Return powercycle_operation if command is one we recognize.

        :param subcommand: equivalent to parsed_args.command
        :param parser: parser used
        :param parsed_args: output of parsing
        :param kwargs: additional args
        :return: None or a Subcommand
        """
        # Only return subcommand if expansion file has been written.
        if not os.path.exists(powercycle_constants.EXPANSIONS_FILE):
            print(f"Did not find {powercycle_constants.EXPANSIONS_FILE}, skipping {subcommand}.")
            return NoOp()

        if subcommand == SetUpEC2Instance.COMMAND:
            return SetUpEC2Instance()
        elif subcommand == TarEC2Artifacts.COMMAND:
            return TarEC2Artifacts()
        elif subcommand == CopyEC2Artifacts.COMMAND:
            return CopyEC2Artifacts()
        elif subcommand == GatherRemoteEventLogs.COMMAND:
            return GatherRemoteEventLogs()
        elif subcommand == GatherRemoteMongoCoredumps.COMMAND:
            return GatherRemoteMongoCoredumps()
        elif subcommand == CopyRemoteMongoCoredumps.COMMAND:
            return CopyRemoteMongoCoredumps()
        elif subcommand == CopyEC2MonitorFiles.COMMAND:
            return CopyEC2MonitorFiles()
        elif subcommand == RunHangAnalyzerOnRemoteInstance.COMMAND:
            return RunHangAnalyzerOnRemoteInstance()
        else:
            return None
