"""setup the remote host for powercycle."""

import os

from buildscripts.resmokelib.powercycle.lib import PowercycleCommand
from buildscripts.resmokelib.powercycle import powercycle_constants
from buildscripts.resmokelib.powercycle.lib.remote_operations import SSHOperation


class SetUpEC2Instance(PowercycleCommand):
    """Set up EC2 instance."""

    COMMAND = "setUpEC2Instance"

    def execute(self) -> None:
        """:return: None."""

        default_retry_count = 2
        retry_count = int(self.expansions.get("set_up_retry_count", default_retry_count))

        # First operation -
        # Create remote_dir.
        group_cmd = f"id -Gn {self.user}"
        _, group = self._call(group_cmd)
        group = group.split(" ")[0]
        user_group = f"{self.user}:{group}"

        remote_dir = powercycle_constants.REMOTE_DIR
        db_path = powercycle_constants.DB_PATH

        set_permission_stmt = "chmod -R 777"
        if self.is_windows():
            set_permission_stmt = "setfacl -s user::rwx,group::rwx,other::rwx"
        cmds = f"{self.sudo} mkdir -p {remote_dir}; {self.sudo} chown -R {user_group} {remote_dir}; {set_permission_stmt} {remote_dir}; ls -ld {remote_dir}"
        cmds = f"{cmds}; {self.sudo} mkdir -p {db_path}; {self.sudo} chown -R {user_group} {db_path}; {set_permission_stmt} {db_path}; ls -ld {db_path}"

        self.remote_op.operation(SSHOperation.SHELL, cmds, retry=True, retry_count=retry_count)

        # Second operation -
        # Copy buildscripts and mongoDB executables to the remote host.
        files = ["etc", "buildscripts", "dist-test/bin"]

        shared_libs = "dist-test/lib"
        if os.path.isdir(shared_libs):
            files.append(shared_libs)

        self.remote_op.operation(SSHOperation.COPY_TO, files, remote_dir, retry=True,
                                 retry_count=retry_count)

        # Third operation -
        # Set up virtualenv on remote.
        venv = powercycle_constants.VIRTUALENV_DIR
        python = "/opt/mongodbtoolchain/v3/bin/python3" if "python" not in self.expansions else self.expansions[
            "python"]

        cmds = f"python_loc=$(which {python})"
        cmds = f"{cmds}; remote_dir={remote_dir}"
        cmds = f"{cmds}; if [ \"Windows_NT\" = \"$OS\" ]; then python_loc=$(cygpath -w $python_loc); remote_dir=$(cygpath -w $remote_dir); fi"
        cmds = f"{cmds}; virtualenv --python $python_loc --system-site-packages {venv}"
        cmds = f"{cmds}; activate=$(find {venv} -name 'activate')"
        cmds = f"{cmds}; . $activate"
        cmds = f"{cmds}; pip3 install -r $remote_dir/etc/pip/powercycle-requirements.txt"

        self.remote_op.operation(SSHOperation.SHELL, cmds, retry=True, retry_count=retry_count)

        # Operation below that enables core dumps is commented out since it causes failures on Ubuntu 18.04.
        # It might be a race condition, so `nohup reboot` command is likely a culprit here.

        # Fourth operation -
        # Enable core dumps on non-Windows remote hosts.
        # The core pattern must specify a director, since mongod --fork will chdir("/")
        # and cannot generate a core dump there (see SERVER-21635).
        # We need to reboot the host for the core limits to take effect.
        # if not self.is_windows():
        #     core_pattern = f"{remote_dir}/dump_%e.%p.core"
        #     sysctl_conf = "/etc/sysctl.conf"
        #     cmds = "ulimit -a"
        #     cmds = f"{cmds}; echo \"{self.user} - core unlimited\" | {self.sudo} tee -a /etc/security/limits.conf"
        #     cmds = f"{cmds}; if [ -f {sysctl_conf} ]"
        #     cmds = f"{cmds}; then grep ^kernel.core_pattern {sysctl_conf}"
        #     cmds = f"{cmds};    if [ $? -eq  0 ]"
        #     cmds = f"{cmds};    then {self.sudo} sed -i \"s,kernel.core_pattern=.*,kernel.core_pattern={core_pattern},\" {sysctl_conf}"
        #     cmds = f"{cmds};    else echo \"kernel.core_pattern={core_pattern}\" | {self.sudo} tee -a {sysctl_conf}"
        #     cmds = f"{cmds};    fi"
        #     cmds = f"{cmds}; else echo Cannot change the core pattern and no core dumps will be generated."
        #     cmds = f"{cmds}; fi"
        #     # The following line for restarting the machine is based on
        #     # https://unix.stackexchange.com/a/349558 in order to ensure the ssh client gets a
        #     # response from the remote machine before it restarts.
        #     cmds = f"{cmds}; nohup {self.sudo} reboot &>/dev/null & exit"
        #     self.remote_op.operation(SSHOperation.SHELL, cmds, retry=True, retry_count=retry_count)

        # Fifth operation -
        # Print the ulimit & kernel.core_pattern
        # if not self.is_windows():
        #     # Always exit successfully, as this is just informational.
        #     cmds = "uptime"
        #     cmds = f"{cmds}; ulimit -a"
        #     cmds = f"{cmds}; if [ -f /sbin/sysctl ]"
        #     cmds = f"{cmds}; then /sbin/sysctl kernel.core_pattern"
        #     cmds = f"{cmds}; fi"
        #
        #     self.remote_op.operation(SSHOperation.SHELL, cmds, retry=True, retry_count=retry_count)

        # Sixth operation -
        # Set up curator to collect system & process stats on remote.
        variant = "windows-64" if self.is_windows() else "linux-32"
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

        self.remote_op.operation(SSHOperation.SHELL, cmds, retry=True, retry_count=retry_count)

        # Seventh operation -
        # Install NotMyFault, used to crash Windows.
        if self.is_windows():
            windows_crash_zip = powercycle_constants.WINDOWS_CRASH_ZIP
            windows_crash_dl = powercycle_constants.WINDOWS_CRASH_DL
            windows_crash_dir = powercycle_constants.WINDOWS_CRASH_DIR

            cmds = f"curl -s -o {windows_crash_zip} {windows_crash_dl}"
            cmds = f"{cmds}; unzip -q {windows_crash_zip} -d {windows_crash_dir}"
            cmds = f"{cmds}; chmod +x {windows_crash_dir}/*.exe"
            self.remote_op.operation(SSHOperation.SHELL, cmds, retry=True, retry_count=retry_count)
