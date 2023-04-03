"""Powercycle test helper functions."""
import atexit
import collections
import copy
import datetime
import distutils.spawn
import importlib
import json
import logging
import os
import pipes
import random
import re
import shlex
import shutil
import signal
import stat
import string
import sys
import tarfile
import tempfile
import threading
import time
import traceback
import urllib.parse
import zipfile

import psutil
import pymongo
import requests
import yaml

from buildscripts.resmokelib.powercycle.lib import remote_operations, execute_cmd, \
    create_temp_executable_file, NamedTempFile
from buildscripts.resmokelib.powercycle import powercycle_config, powercycle_constants

# See https://docs.python.org/2/library/sys.html#sys.platform
from buildscripts.resmokelib.powercycle.lib.services import WindowsService, PosixService
from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path

_IS_WINDOWS = sys.platform == "win32" or sys.platform == "cygwin"
_IS_LINUX = sys.platform.startswith("linux")
_IS_DARWIN = sys.platform == "darwin"


def _try_import(module, name=None):
    """Attempt to import a module and add it as a global variable.

    If the import fails, then this function doesn't trigger an exception.
    """
    try:
        module_name = module if not name else name
        globals()[module_name] = importlib.import_module(module)
    except ImportError:
        pass


if _IS_WINDOWS:
    # These modules are used on both sides for dumping python stacks.
    import win32api
    import win32event

    # These modules are used on the 'server' side.
    _try_import("ntsecuritycon")
    _try_import("pywintypes")
    _try_import("winerror")
    _try_import("win32file")
    _try_import("win32security")
    _try_import("win32service")
    _try_import("win32serviceutil")

LOGGER = logging.getLogger(__name__)

ONE_HOUR_SECS = 60 * 60

REPORT_JSON = {}  # type: ignore
REPORT_JSON_FILE = ""
REPORT_JSON_SUCCESS = False

EXIT_YML = {"exit_code": 0}  # type: ignore
EXIT_YML_FILE = ""


def local_exit(code):
    """Capture exit code and invoke sys.exit."""
    EXIT_YML["exit_code"] = code
    sys.exit(code)


def exit_handler():
    """Exit handler to generate report.json, kill spawned processes, delete temporary files."""

    if REPORT_JSON:
        LOGGER.debug("Exit handler: Updating report file %s", REPORT_JSON_FILE)
        try:
            test_start = REPORT_JSON["results"][0]["start"]
            test_end = int(time.time())
            test_time = test_end - test_start
            if REPORT_JSON_SUCCESS:
                failures = 0
                status = "pass"
                exit_code = 0
            else:
                failures = 1
                status = "fail"
                exit_code = 1
            REPORT_JSON["failures"] = failures
            REPORT_JSON["results"][0]["status"] = status
            REPORT_JSON["results"][0]["exit_code"] = exit_code
            REPORT_JSON["results"][0]["end"] = test_end
            REPORT_JSON["results"][0]["elapsed"] = test_time
            with open(REPORT_JSON_FILE, "w") as jstream:
                json.dump(REPORT_JSON, jstream)
            LOGGER.debug("Exit handler: report file contents %s", REPORT_JSON)
        except:  # pylint: disable=bare-except
            pass

    if EXIT_YML_FILE:
        LOGGER.debug("Exit handler: Saving exit file %s", EXIT_YML_FILE)
        try:
            with open(EXIT_YML_FILE, "w") as yaml_stream:
                yaml.safe_dump(EXIT_YML, yaml_stream)
            LOGGER.debug("Exit handler: report file contents %s", EXIT_YML)
        except:  # pylint: disable=bare-except
            pass

    LOGGER.debug("Exit handler: Killing processes")
    try:
        Processes.kill_all()
        LOGGER.debug("Exit handler: Killing processes finished")
    except:  # pylint: disable=bare-except
        pass

    LOGGER.debug("Exit handler: Cleaning up temporary files")
    try:
        NamedTempFile.delete_all()
        LOGGER.debug("Exit handler: Cleaning up temporary files finished")
    except:  # pylint: disable=bare-except
        pass


def register_signal_handler(handler):
    """Register the signal handler."""

    def _handle_set_event(event_handle, handler):
        """Event object handler that will dump the stacks of all threads for Windows."""
        while True:
            try:
                # Wait for task time out to dump stacks.
                ret = win32event.WaitForSingleObject(event_handle, win32event.INFINITE)
                if ret != win32event.WAIT_OBJECT_0:
                    LOGGER.error("_handle_set_event WaitForSingleObject failed: %d", ret)
                    return
            except win32event.error as err:
                LOGGER.error("Exception from win32event.WaitForSingleObject with error: %s", err)
            else:
                handler(None, None)

    if _IS_WINDOWS:
        # Create unique event_name.
        event_name = "Global\\Mongo_Python_{:d}".format(os.getpid())
        LOGGER.debug("Registering event %s", event_name)

        try:
            security_attributes = None
            manual_reset = False
            initial_state = False
            task_timeout_handle = win32event.CreateEvent(security_attributes, manual_reset,
                                                         initial_state, event_name)
        except win32event.error as err:
            LOGGER.error("Exception from win32event.CreateEvent with error: %s", err)
            return

        # Register to close event object handle on exit.
        atexit.register(win32api.CloseHandle, task_timeout_handle)

        # Create thread.
        event_handler_thread = threading.Thread(
            target=_handle_set_event,
            kwargs={"event_handle": task_timeout_handle,
                    "handler": handler}, name="windows_event_handler_thread")
        event_handler_thread.daemon = True
        event_handler_thread.start()
    else:
        # Otherwise register a signal handler for SIGUSR1.
        signal_num = signal.SIGUSR1
        signal.signal(signal_num, handler)


def dump_stacks_and_exit(signum, frame):  # pylint: disable=unused-argument
    """Provide a handler that will dump the stacks of all threads."""
    LOGGER.info("Dumping stacks!")

    sb = []
    frames = sys._current_frames()  # pylint: disable=protected-access
    sb.append("Total threads: {}\n".format(len(frames)))
    sb.append("")

    for thread_id in frames:
        stack = frames[thread_id]
        sb.append("Thread {}:".format(thread_id))
        sb.append("".join(traceback.format_stack(stack)))

    LOGGER.info("".join(sb))

    if _IS_WINDOWS:
        exit_handler()
        os._exit(1)  # pylint: disable=protected-access
    else:
        sys.exit(1)


def kill_process(parent, kill_children=True):
    """Kill a process, and optionally it's children, by it's pid. Returns 0 if successful."""
    try:
        # parent.children() implicitly calls parent.is_running(), which raises a
        # psutil.NoSuchProcess exception if the creation time for the process with pid=parent.pid is
        # different than parent.create_time(). We can reliably detect pid reuse this way because
        # 'parent' is the same psutil.Process instance returned by start_cmd() and therefore has an
        # accurate notion of the creation time.
        procs = parent.children(recursive=True) if kill_children else []
    except psutil.NoSuchProcess:
        LOGGER.warning("Could not kill process %d, as it no longer exists", parent.pid)
        return 0

    procs.append(parent)

    for proc in procs:
        try:
            LOGGER.debug("Killing process '%s' pid %d", proc.name(), proc.pid)
            proc.kill()
        except psutil.NoSuchProcess:
            LOGGER.warning("Could not kill process %d, as it no longer exists", proc.pid)

    _, alive = psutil.wait_procs(procs, timeout=30, callback=None)
    if alive:
        for proc in alive:
            LOGGER.error("Process %d still alive!", proc.pid)
    return 0


def kill_processes(procs, kill_children=True):
    """Kill a list of processes and optionally it's children."""
    for proc in procs:
        LOGGER.debug("Starting kill of parent process %d", proc.pid)
        kill_process(proc, kill_children=kill_children)
        ret = proc.wait()
        LOGGER.debug("Finished kill of parent process %d has return code of %d", proc.pid, ret)


def get_extension(filename):
    """Return the extension of a file."""
    return os.path.splitext(filename)[-1]


def executable_extension():
    """Return executable file extension."""
    if _IS_WINDOWS:
        return ".exe"
    return ""


def abs_path(path):
    """Return absolute path for 'path'. Raises an exception on failure."""
    if _IS_WINDOWS:
        # Get the Windows absolute path.
        cmd = "cygpath -wa {}".format(path)
        ret, output = execute_cmd(cmd, use_file=True)
        if ret:
            raise Exception("Command \"{}\" failed with code {} and output message: {}".format(
                cmd, ret, output))
        return output.rstrip().replace("\\", "/")
    return os.path.abspath(os.path.normpath(path))


def symlink_dir(source_dir, dest_dir):
    """Symlink the 'dest_dir' to 'source_dir'."""
    if _IS_WINDOWS:
        win32file.CreateSymbolicLink(  # pylint: disable=undefined-variable
            dest_dir, source_dir, win32file.SYMBOLIC_LINK_FLAG_DIRECTORY)  # pylint: disable=undefined-variable
    else:
        os.symlink(source_dir, dest_dir)


def get_bin_dir(root_dir):
    """Locate the 'bin' directory within 'root_dir' tree."""
    for root, dirs, _ in os.walk(root_dir):
        if "bin" in dirs:
            return os.path.join(root, "bin")
    return None


def start_cmd(cmd, use_file=False):
    """Start command and returns proc instance from Popen."""

    orig_cmd = ""
    # Multi-commands need to be written to a temporary file to execute on Windows.
    # This is due to complications with invoking Bash in Windows.
    if use_file:
        orig_cmd = cmd
        temp_file = create_temp_executable_file(cmd)
        # The temporary file name will have '\' on Windows and needs to be converted to '/'.
        cmd = "bash -c {}".format(temp_file.replace("\\", "/"))

    # If 'cmd' is specified as a string, convert it to a list of strings.
    if isinstance(cmd, str):
        cmd = shlex.split(cmd)

    if use_file:
        LOGGER.debug("Executing '%s', tempfile contains: %s", cmd, orig_cmd)
    else:
        LOGGER.debug("Executing '%s'", cmd)

    # We use psutil.Popen() rather than subprocess.Popen() in order to cache the creation time of
    # the process. This enables us to reliably detect pid reuse in kill_process().
    proc = psutil.Popen(cmd, close_fds=True)
    LOGGER.debug("Spawned process %s pid %d", proc.name(), proc.pid)

    return proc


def get_user_host(user_host):
    """Return a tuple (user, host) from the user_host string."""
    if "@" in user_host:
        return tuple(user_host.split("@"))
    return None, user_host


def parse_options(options):
    """Parse options and returns a dict.

    Since there are options which can be specifed with a short('-') or long
    ('--') form, we preserve that in key map as {option_name: (value, form)}.
    """
    options_map = collections.defaultdict(list)
    opts = shlex.split(options)
    opt_name = None
    opt_form = None
    for opt in opts:
        # Handle options which could start with "-" or "--".
        if opt.startswith("-"):
            opt_idx = 2 if opt[1] == "-" else 1
            opt_form = opt[:opt_idx]
            eq_idx = opt.find("=")
            if eq_idx == -1:
                opt_name = opt[opt_idx:]
                options_map[opt_name] = (None, opt_form)
            else:
                opt_name = opt[opt_idx:eq_idx]
                options_map[opt_name] = (opt[eq_idx + 1:], opt_form)
                opt_name = None
        elif opt_name:
            options_map[opt_name] = (opt, opt_form)
    return options_map


def download_file(url, file_name, download_retries=5):
    """Return True if download was successful, raise error if download fails."""

    LOGGER.info("Downloading %s to %s", url, file_name)
    while download_retries > 0:

        with requests.Session() as session:
            adapter = requests.adapters.HTTPAdapter(max_retries=download_retries)
            session.mount(url, adapter)
            response = session.get(url, stream=True)
            response.raise_for_status()

            with open(file_name, "wb") as file_handle:
                try:
                    for block in response.iter_content(1024 * 1000):
                        file_handle.write(block)
                except requests.exceptions.ChunkedEncodingError as err:
                    download_retries -= 1
                    if download_retries == 0:
                        raise Exception("Incomplete download for URL {}: {}".format(url, err))
                    continue

        # Check if file download was completed.
        if "Content-length" in response.headers:
            url_content_length = int(response.headers["Content-length"])
            file_size = os.path.getsize(file_name)
            # Retry download if file_size has an unexpected size.
            if url_content_length != file_size:
                download_retries -= 1
                if download_retries == 0:
                    raise Exception("Downloaded file size ({} bytes) doesn't match content length"
                                    "({} bytes) for URL {}".format(file_size, url_content_length,
                                                                   url))
                continue

        return True

    raise Exception("Unknown download problem for {} to file {}".format(url, file_name))


def install_tarball(tarball, root_dir):
    """Unzip and install 'tarball' into 'root_dir'."""

    LOGGER.info("Installing %s to %s", tarball, root_dir)
    output = ""
    extensions = [".msi", ".tgz", ".zip"]
    ext = get_extension(tarball)
    if ext == ".tgz":
        with tarfile.open(tarball, "r:gz") as tar_handle:
            tar_handle.extractall(path=root_dir)
            output = "Unzipped {} to {}: {}".format(tarball, root_dir, tar_handle.getnames())
        ret = 0
    elif ext == ".zip":
        with zipfile.ZipFile(tarball, "r") as zip_handle:
            zip_handle.extractall(root_dir)
            output = "Unzipped {} to {}: {}".format(tarball, root_dir, zip_handle.namelist())
        ret = 0
    elif ext == ".msi":
        if not _IS_WINDOWS:
            raise Exception("Unsupported platform for MSI install")
        tmp_dir = tempfile.mkdtemp(dir="c:\\")
        # Change the ownership to $USER: as ssh over Cygwin does not preserve privileges
        # (see https://cygwin.com/ml/cygwin/2004-09/msg00087.html).
        cmds = """
            msiexec /a {tarball} /qn TARGETDIR="{tmp_dir}" /l msi.log ;
            if [ $? -ne 0 ]; then
                echo "msiexec failed to extract from {tarball}" ;
                echo See msi.log ;
                exit 1 ;
            fi ;
            mv "{tmp_dir}"/* "{root_dir}" ;
            chown -R $USER: "{root_dir}" ;
            chmod -R 777 "{root_dir}" ;
            winsysdir=c:/Windows/System32 ;
            pushd "{root_dir}/System64" ;
            for dll in * ;
            do
               if [ ! -f $winsysdir/$dll ]; then
                  echo "File $winsysdir/$dll does not exist, copying from $(pwd)" ;
                  cp $dll $winsysdir/ ;
               else
                  echo "File $winsysdir/$dll already exists" ;
               fi ;
            done ;
            popd ;
            """.format(tarball=tarball, tmp_dir=tmp_dir, root_dir=root_dir)
        ret, output = execute_cmd(cmds, use_file=True)
        shutil.rmtree(tmp_dir)
    else:
        raise Exception("Unsupported file extension to unzip {},"
                        " supported extensions are {}".format(tarball, extensions))

    LOGGER.debug(output)
    if ret:
        raise Exception("Failed to install tarball {}, {}".format(tarball, output))


def chmod_x_binaries(bin_dir):
    """Change all file permissions in 'bin_dir' to executable for everyone."""

    files = os.listdir(bin_dir)
    LOGGER.debug("chmod +x %s %s", bin_dir, files)
    for dir_file in files:
        bin_file = os.path.join(bin_dir, dir_file)
        os_st = os.stat(bin_file)
        os.chmod(bin_file, os_st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def chmod_w_file(chmod_file):
    """Change the permission for 'chmod_file' to '+w' for everyone."""

    LOGGER.debug("chmod +w %s", chmod_file)
    if _IS_WINDOWS:
        # The os package cannot set the directory to '+w', so we use win32security.
        # See https://stackoverflow.com/
        #       questions/12168110/setting-folder-permissions-in-windows-using-python
        # pylint: disable=undefined-variable,unused-variable
        user, domain, sec_type = win32security.LookupAccountName("", "Everyone")
        file_sd = win32security.GetFileSecurity(chmod_file, win32security.DACL_SECURITY_INFORMATION)
        dacl = file_sd.GetSecurityDescriptorDacl()
        dacl.AddAccessAllowedAce(win32security.ACL_REVISION, ntsecuritycon.FILE_GENERIC_WRITE, user)
        file_sd.SetSecurityDescriptorDacl(1, dacl, 0)
        win32security.SetFileSecurity(chmod_file, win32security.DACL_SECURITY_INFORMATION, file_sd)
        # pylint: enable=undefined-variable,unused-variable
    else:
        os.chmod(chmod_file, os.stat(chmod_file) | stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)


def set_windows_bootstatuspolicy():
    """For Windows hosts that are physical, this prevents boot to prompt after failure."""

    LOGGER.info("Setting bootstatuspolicy to ignoreallfailures & boot timeout to 5 seconds")
    cmds = """
        echo 'Setting bootstatuspolicy to ignoreallfailures & boot timeout to 5 seconds' ;
        bcdedit /set {default} bootstatuspolicy ignoreallfailures ;
        bcdedit /set {current} bootstatuspolicy ignoreallfailures ;
        bcdedit /timeout 5"""
    ret, output = execute_cmd(cmds, use_file=True)
    return ret, output


def _do_install_mongod(bin_dir=None, tarball_url="latest", root_dir=None):
    """Set up 'root_dir'/bin to contain MongoDB binaries.

    If 'bin_dir' is specified, then symlink it to 'root_dir'/bin.
    Otherwise, download 'tarball_url' and symlink it's bin to 'root_dir'/bin.

    If 'bin_dir' is specified, skip download and create symlink
    from 'bin_dir' to 'root_dir'/bin.
    """

    LOGGER.debug("install_mongod: %s %s %s", bin_dir, tarball_url, root_dir)
    # Create 'root_dir', if it does not exist.
    root_bin_dir = os.path.join(root_dir, "bin")
    if not os.path.isdir(root_dir):
        LOGGER.info("install_mongod: creating %s", root_dir)
        os.makedirs(root_dir)

    # Symlink the 'bin_dir', if it's specified, to 'root_bin_dir'
    if bin_dir and os.path.isdir(bin_dir):
        symlink_dir(bin_dir, root_bin_dir)
        return

    if tarball_url == "latest":
        # TODO SERVER-31021: Support all platforms.
        if _IS_WINDOWS:
            # MSI default:
            # https://fastdl.mongodb.org/win32/mongodb-win32-x86_64-2008plus-ssl-latest-signed.msi
            tarball_url = (
                "https://fastdl.mongodb.org/win32/mongodb-win32-x86_64-2008plus-ssl-latest.zip")
        elif _IS_LINUX:
            tarball_url = "https://fastdl.mongodb.org/linux/mongodb-linux-x86_64-latest.tgz"

    tarball = os.path.split(urllib.parse.urlsplit(tarball_url).path)[-1]
    download_file(tarball_url, tarball)
    install_tarball(tarball, root_dir)
    chmod_x_binaries(get_bin_dir(root_dir))

    # Symlink the bin dir from the tarball to 'root_bin_dir'.
    # Since get_bin_dir returns an abolute path, we need to remove 'root_dir'
    tarball_bin_dir = get_bin_dir(root_dir).replace("{}/".format(root_dir), "")
    LOGGER.debug("Symlink %s to %s", tarball_bin_dir, root_bin_dir)
    symlink_dir(tarball_bin_dir, root_bin_dir)


def get_boot_datetime(uptime_string):
    """Return the datetime value of boot_time from formatted print_uptime 'uptime_string'.

    Return -1 if it is not found in 'uptime_string'.
    """
    match = re.search(r"last booted (.*), up", uptime_string)
    if match:
        return datetime.datetime(
            *list(map(int, list(map(float, re.split("[ :-]",
                                                    match.groups()[0]))))))
    return -1


def print_uptime():
    """Print the last time the system was booted, and the uptime (in seconds)."""
    boot_time_epoch = psutil.boot_time()
    boot_time = datetime.datetime.fromtimestamp(boot_time_epoch).strftime('%Y-%m-%d %H:%M:%S.%f')
    uptime = int(time.time() - boot_time_epoch)
    LOGGER.info("System was last booted %s, up %d seconds", boot_time, uptime)


def call_remote_operation(local_ops, remote_python, script_name, client_args, operation):
    """Call the remote operation and return tuple (ret, ouput)."""
    client_call = f"{remote_python} {script_name} {client_args} {operation}"
    ret, output = local_ops.shell(client_call)
    if local_ops.ssh_error(output):
        ssh_failure_exit(ret, output)
    return ret, output


class Processes(object):
    """Class to create and kill spawned processes."""

    _PROC_LIST = []  # type: ignore

    @classmethod
    def create(cls, cmds, use_file=False):
        """Create a spawned process."""
        proc = start_cmd(cmds, use_file=use_file)
        cls._PROC_LIST.append(proc)

    @classmethod
    def kill(cls, proc):
        """Kill a spawned process and all it's children."""
        kill_processes([proc], kill_children=True)
        cls._PROC_LIST.remove(proc)

    @classmethod
    def kill_all(cls):
        """Kill all spawned processes."""
        procs = copy.copy(cls._PROC_LIST)
        for proc in procs:
            cls.kill(proc)


class MongodControl(object):
    """Control mongod process."""

    def __init__(self, bin_dir, db_path, log_path, port, options=None):
        """Initialize MongodControl."""
        self.process_name = "mongod{}".format(executable_extension())

        self.bin_dir = bin_dir
        if self.bin_dir:
            self.bin_path = os.path.join(self.bin_dir, self.process_name)
            if not os.path.isfile(self.bin_path):
                LOGGER.error("File %s does not exist.", self.bin_path)
        else:
            self.bin_path = None

        self.options_map = parse_options(options)
        self.db_path = db_path
        self.set_mongod_option("dbpath", db_path)
        self.log_path = log_path
        self.set_mongod_option("logpath", log_path)
        self.set_mongod_option("logappend")
        self.port = port
        self.set_mongod_option("port", port)
        self.set_mongod_option("bind_ip", "0.0.0.0")
        if _IS_WINDOWS:
            self.set_mongod_option("service")
            self._service = WindowsService
        else:
            self.set_mongod_option("fork")
            self._service = PosixService
        # After mongod has been installed, self.bin_path is defined.
        if self.bin_path:
            self.service = self._service("mongod-powercycle-test", self.bin_path,
                                         self.mongod_options(), db_path)

    def set_mongod_option(self, option, option_value=None, option_form="--"):
        """Set mongod command line option."""
        self.options_map[option] = (option_value, option_form)

    def get_mongod_option(self, option):
        """Return tuple of (value, form)."""
        return self.options_map[option]

    def get_mongod_service(self):
        """Return the service object used to control mongod."""
        return self.service

    def mongod_options(self):
        """Return string of mongod options, which can be used when invoking mongod."""
        opt_string = ""
        for opt_name in self.options_map:
            opt_val, opt_form = self.options_map[opt_name]
            opt_string += " {}{}".format(opt_form, opt_name)
            if opt_val:
                opt_string += " {}".format(opt_val)
        return opt_string

    def install(self, root_dir, tarball_url):
        """Return tuple (ret, ouput)."""
        # Install mongod, if 'root_dir' does not exist.
        if os.path.isdir(root_dir):
            LOGGER.warning("Root dir %s already exists", root_dir)
        else:
            _do_install_mongod(bin_dir=self.bin_dir, tarball_url=tarball_url, root_dir=root_dir)
        self.bin_dir = get_bin_dir(root_dir)
        if not self.bin_dir:
            ret, output = execute_cmd("ls -lR '{}'".format(root_dir), use_file=True)
            LOGGER.debug(output)
            return 1, "No bin dir can be found under {}".format(root_dir)
        self.bin_path = os.path.join(self.bin_dir, self.process_name)
        # We need to instantiate the Service when installing, since the bin_path
        # is only known after install_mongod runs.
        self.service = self._service("mongod-powercycle-test", self.bin_path, self.mongod_options(),
                                     db_path=None)
        ret, output = self.service.create()
        return ret, output

    def uninstall(self):
        """Return tuple (ret, ouput)."""
        return self.service.delete()

    @staticmethod
    def cleanup(root_dir):
        """Return tuple (ret, ouput)."""
        shutil.rmtree(root_dir, ignore_errors=True)
        return 0, None

    def start(self):
        """Return tuple (ret, ouput)."""
        return self.service.start()

    def update(self):
        """Return tuple (ret, ouput)."""
        return self.service.update()

    def stop(self, timeout=0):
        """Return tuple (ret, ouput)."""
        return self.service.stop(timeout)

    def status(self):
        """Return status of the process."""
        return self.service.status()

    def get_pids(self):
        """Return list of pids for process."""
        return self.service.get_pids()


def ssh_failure_exit(code, output):
    """Exit on ssh failure with code."""
    EXIT_YML["ec2_ssh_failure"] = output
    local_exit(code)


def verify_remote_access(remote_op):
    """Exit if the remote host is not accessible and save result to YML file."""
    if not remote_op.access_established():
        code, output = remote_op.access_info()
        LOGGER.error("Exiting, unable to establish access (%d): %s", code, output)
        ssh_failure_exit(code, output)


class LocalToRemoteOperations(object):
    """Local operations handler class for sending commands to the remote host.

    Return (return code, output).
    """

    def __init__(self, user_host, ssh_connection_options=None, ssh_options=None,
                 shell_binary="/bin/bash", use_shell=False, access_retry_count=5):
        """Initialize LocalToRemoteOperations."""

        self.remote_op = remote_operations.RemoteOperations(
            user_host=user_host, ssh_connection_options=ssh_connection_options,
            ssh_options=ssh_options, shell_binary=shell_binary, use_shell=use_shell,
            ignore_ret=True, access_retry_count=access_retry_count)

    def shell(self, cmds, remote_dir=None):
        """Return tuple (ret, output) from performing remote shell operation."""
        return self.remote_op.shell(cmds, remote_dir)

    def copy_from(self, files, remote_dir=None):
        """Return tuple (ret, output) from performing remote copy_to operation."""
        return self.remote_op.copy_from(files, remote_dir)

    def copy_to(self, files, remote_dir=None):
        """Return tuple (ret, output) from performing remote copy_from operation."""
        return self.remote_op.copy_to(files, remote_dir)

    def access_established(self):
        """Return True if remote access has been established."""
        return self.remote_op.access_established()

    def ssh_error(self, output):
        """Return True if 'output' contains an ssh error."""
        return self.remote_op.ssh_error(output)

    def access_info(self):
        """Return the return code and output buffer from initial access attempt(s)."""
        return self.remote_op.access_info()


def remote_handler(options, task_config, root_dir):
    """Remote operations handler executes all remote operations on the remote host.

    These operations are invoked on the remote host's copy of this script.
    Only one operation can be performed at a time.
    """

    # Set 'root_dir' to absolute path.
    root_dir = abs_path(root_dir)
    if not options.remote_operations:
        raise ValueError("No remote operation specified.")

    print_uptime()
    LOGGER.info("Operations to perform %s", options.remote_operations)
    host = options.host if options.host else "localhost"
    host_port = "{}:{}".format(host, options.port)

    mongod_options = task_config.mongod_options
    if task_config.repl_set:
        mongod_options = f"{mongod_options} --replSet {task_config.repl_set}"

    # For MongodControl, the file references should be fully specified.
    bin_dir = os.path.join(abs_path(powercycle_constants.REMOTE_DIR), "bin")
    db_path = abs_path(powercycle_constants.DB_PATH)
    log_path = abs_path(powercycle_constants.LOG_PATH)

    mongod = MongodControl(bin_dir=bin_dir, db_path=db_path, log_path=log_path, port=options.port,
                           options=mongod_options)

    mongo_client_opts = get_mongo_client_args(host=host, port=options.port, task_config=task_config)

    # Perform the sequence of operations specified. If any operation fails then return immediately.
    for operation in options.remote_operations:
        ret = 0

        def noop():
            pass

        # This is the internal "crash" mechanism, which is executed on the remote host.
        def crash_server():
            ret, _ = internal_crash()
            # An internal crash on Windows is not immediate
            try:
                LOGGER.info("Waiting after issuing internal crash!")
                time.sleep(60)
            except IOError:
                pass

            return ret

        def kill_mongod():
            # Unconditional kill of mongod.
            ret, output = _do_kill_mongod()
            if ret:
                LOGGER.error("kill_mongod failed %s", output)
                return ret
            # Ensure the mongod service is not in a running state. WT can take 10+ minutes to
            # cleanly shutdown. Prefer to hit the evergreen timeout which will run the hang
            # analyzer.
            ret, output = mongod.stop(timeout=2 * ONE_HOUR_SECS)
            LOGGER.info(output)
            status = mongod.status()
            if status != "stopped":
                LOGGER.error("Unable to stop the mongod service, in state '%s'", status)
                ret = 1

            return ret

        def install_mongod():
            ret, output = mongod.install(root_dir, options.tarball_url)
            LOGGER.info(output)

            # Create mongod's dbpath, if it does not exist.
            if not os.path.isdir(db_path):
                os.makedirs(db_path)

            # Create mongod's logpath directory, if it does not exist.
            log_dir = os.path.dirname(log_path)
            if not os.path.isdir(log_dir):
                os.makedirs(log_dir)

            # Windows special handling.
            if _IS_WINDOWS:
                # The os package cannot set the directory to '+w'
                # See https://docs.python.org/2/library/os.html#os.chmod
                chmod_w_file(db_path)
                chmod_w_file(log_dir)
                # Disable boot prompt after system crash.
                ret, output = set_windows_bootstatuspolicy()
                LOGGER.info(output)

            return ret

        def start_mongod():
            # Always update the service before starting, as options might have changed.
            ret, output = mongod.update()
            LOGGER.info(output)
            ret, output = mongod.start()
            LOGGER.info(output)
            if ret:
                LOGGER.error("Failed to start mongod on port %d: %s", options.port, output)
                return ret
            LOGGER.info("Started mongod running on port %d pid %s", options.port, mongod.get_pids())
            mongo = pymongo.MongoClient(**mongo_client_opts)
            # Limit retries to a reasonable value
            for _ in range(100):
                try:
                    build_info = mongo.admin.command("buildinfo")
                    server_status = mongo.admin.command("serverStatus")
                    break
                except pymongo.errors.AutoReconnect:
                    pass
            LOGGER.info("Server buildinfo: %s", build_info)
            LOGGER.info("Server serverStatus: %s", server_status)
            if task_config.repl_set:
                ret = mongo_reconfig_replication(mongo, host_port, task_config.repl_set)

            return ret

        def stop_mongod():
            _, output = mongod.stop()
            LOGGER.info(output)
            return wait_for_mongod_shutdown(mongod)

        def shutdown_mongod():
            mongo = pymongo.MongoClient(**mongo_client_opts)
            try:
                mongo.admin.command("shutdown", force=True)
            except pymongo.errors.AutoReconnect:
                pass
            return wait_for_mongod_shutdown(mongod)

        def rsync_data():
            rsync_dir, new_rsync_dir = options.rsync_dest
            ret, output = rsync(powercycle_constants.DB_PATH, rsync_dir,
                                powercycle_constants.RSYNC_EXCLUDE_FILES)
            if output:
                LOGGER.info(output)
            # Rename the rsync_dir only if it has a different name than new_rsync_dir.
            if ret == 0 and rsync_dir != new_rsync_dir:
                LOGGER.info("Renaming directory %s to %s", rsync_dir, new_rsync_dir)
                os.rename(abs_path(rsync_dir), abs_path(new_rsync_dir))

            return ret

        def seed_docs():
            mongo = pymongo.MongoClient(**mongo_client_opts)
            return mongo_seed_docs(mongo, powercycle_constants.DB_NAME,
                                   powercycle_constants.COLLECTION_NAME, task_config.seed_doc_num)

        def set_fcv():
            mongo = pymongo.MongoClient(**mongo_client_opts)
            try:
                ret = mongo.admin.command("setFeatureCompatibilityVersion", task_config.fcv,
                                          confirm=True)
                ret = 0 if ret["ok"] == 1 else 1
            except pymongo.errors.OperationFailure as err:
                LOGGER.error("%s", err)
                ret = err.code

            return ret

        def check_disk():
            ret = 0
            if _IS_WINDOWS:
                partitions = psutil.disk_partitions()
                for part in partitions:
                    if part.fstype != "NTFS" or part.mountpoint == "C:\\":
                        # Powercycle testing in Evergreen only writes to the D: and E: drives on the
                        # remote machine. We skip running the chkdsk command on the C: drive because
                        # it sometimes fails with a "Snapshot was deleted" error. We assume the
                        # system drive is functioning properly because the remote machine rebooted
                        # fine anyway.
                        continue

                    # The chkdsk command won't accept the drive if it has a trailing backslash.
                    # We use os.path.splitdrive()[0] to transform 'C:\\' into 'C:'.
                    drive_letter = os.path.splitdrive(part.mountpoint)[0]
                    LOGGER.info("Running chkdsk command for %s drive", drive_letter)
                    cmds = f"chkdsk '{drive_letter}'"
                    ret, output = execute_cmd(cmds, use_file=True)
                    LOGGER.warning("chkdsk command for %s drive exited with code %d:\n%s",
                                   drive_letter, ret, output)

                    if ret != 0:
                        return ret
            return ret

        op_map = {
            "noop": noop, "crash_server": crash_server, "kill_mongod": kill_mongod,
            "install_mongod": install_mongod, "start_mongod": start_mongod, "stop_mongod":
                stop_mongod, "shutdown_mongod": shutdown_mongod, "rsync_data": rsync_data,
            "seed_docs": seed_docs, "set_fcv": set_fcv, "check_disk": check_disk
        }

        if operation not in op_map:
            LOGGER.error("Unsupported remote option specified '%s'", operation)
            ret = 1
        else:
            ret = op_map[operation]()

        if ret:
            return ret

    return 0


def get_backup_path(path, loop_num):
    """Return the backup path based on the loop_num."""
    return re.sub("-{}$".format(loop_num - 1), "-{}".format(loop_num), path)


def rsync(src_dir, dest_dir, exclude_files=None):
    """Rsync 'src_dir' to 'dest_dir'."""
    # Note rsync on Windows requires a Unix-style directory.
    exclude_options = ""
    exclude_str = ""
    if exclude_files:
        exclude_str = " (excluding {})".format(exclude_files)
        if isinstance(exclude_files, str):
            exclude_files = [exclude_files]
        for exclude_file in exclude_files:
            exclude_options = "{} --exclude '{}'".format(exclude_options, exclude_file)

    LOGGER.info("Rsync'ing %s to %s%s", src_dir, dest_dir, exclude_str)
    if not distutils.spawn.find_executable("rsync"):
        return 1, "No rsync exists on the host, not rsync'ing"

    # We retry running the rsync command up to 'max_attempts' times in order to work around how it
    # sporadically fails under cygwin on Windows Server 2016 with a "No medium found" error message.
    max_attempts = 5
    rsync_output = None
    ret = -1
    for attempt in range(1, max_attempts + 1):
        rsync_cmd = f"rsync -va --delete --quiet {exclude_options} {src_dir} {dest_dir}"
        ret, rsync_output = execute_cmd(rsync_cmd)

        if ret == 0 or "No medium found" not in rsync_output:
            break

        LOGGER.warning("[%d/%d] rsync command failed (code=%d): %s", attempt, max_attempts, ret,
                       rsync_output)

        # If the rsync command failed with an "No medium found" error message, then we log some
        # basic information about the /log mount point.
        diag_cmds = "ls -ld /data/db /log; df"
        _, diag_output = execute_cmd(diag_cmds, use_file=True)
        LOGGER.info("Output from running '%s':\n%s", diag_cmds, diag_output)

    return ret, rsync_output


def _do_kill_mongod():
    """Kill all mongod processes uncondtionally."""
    if _IS_WINDOWS:
        cmds = "taskkill /f /im mongod.exe"
    else:
        cmds = "pkill -9 mongod"
    ret, output = execute_cmd(cmds, use_file=True)
    return ret, output


def internal_crash():
    """Internally crash the host this excutes on."""

    # Windows can use NotMyFault to immediately crash itself, if it's been installed.
    # See https://docs.microsoft.com/en-us/sysinternals/downloads/notmyfault
    # Otherwise it's better to use an external mechanism instead.
    if _IS_WINDOWS:
        cmds = powercycle_constants.WINDOWS_CRASH_CMD
        ret, output = execute_cmd(cmds, use_file=True)
        return ret, output
    else:
        # These operations simulate a console boot and require root privileges, see:
        # - http://www.linuxjournal.com/content/rebooting-magic-way
        # - https://www.mjmwired.net/kernel/Documentation/sysrq.txt
        # These file operations could be performed natively,
        # however since they require root (or sudo), we prefer to do them
        # in a subprocess call to isolate them and not require the invocation
        # of this script to be with sudo.
        # Code to perform natively:
        #   with open("/proc/sys/kernel/sysrq", "w") as f:
        #       f.write("1\n")
        #   with open("/proc/sysrq-trigger", "w") as f:
        #       f.write("b\n")
        sudo = "/usr/bin/sudo"
        cmds = f"""
            echo "Server crashing now" | {sudo} wall ;
            echo 1 | {sudo} tee /proc/sys/kernel/sysrq ;
            echo b | {sudo} tee /proc/sysrq-trigger"""
        ret, output = execute_cmd(cmds, use_file=True)
    LOGGER.debug(output)
    return 1, "Crash did not occur"


def crash_server_or_kill_mongod(task_config, crash_canary, local_ops, script_name, client_args):
    """Crash server or kill mongod and optionally write canary doc. Return tuple (ret, output)."""

    crash_wait_time = powercycle_constants.CRASH_WAIT_TIME + random.randint(
        0, powercycle_constants.CRASH_WAIT_TIME_JITTER)
    message_prefix = "Killing mongod" if task_config.crash_method == "kill" else "Crashing server"
    LOGGER.info("%s in %d seconds", message_prefix, crash_wait_time)
    time.sleep(crash_wait_time)

    if task_config.crash_method == "internal" or task_config.crash_method == "kill":
        crash_cmd = "crash_server" if task_config.crash_method == "internal" else "kill_mongod"
        crash_func = local_ops.shell
        remote_python = get_remote_python()
        crash_args = [f"{remote_python} {script_name} {client_args} --remoteOperation {crash_cmd}"]

    else:
        message = "Unsupported crash method '{}' provided".format(task_config.crash_method)
        LOGGER.error(message)
        return 1, message

    # Invoke the crash canary function, right before crashing the server.
    if crash_canary:
        crash_canary["function"](*crash_canary["args"])
    ret, output = crash_func(*crash_args)
    LOGGER.info(output)
    return ret, output


def wait_for_mongod_shutdown(mongod_control, timeout=2 * ONE_HOUR_SECS):
    """Wait for for mongod to shutdown; return 0 if shutdown occurs within 'timeout', else 1."""
    start = time.time()
    status = mongod_control.status()
    while status != "stopped":
        if time.time() - start >= timeout:
            LOGGER.error("The mongod process has not stopped, current status is %s", status)
            return 1
        LOGGER.info("Waiting for mongod process to stop, current status is %s ", status)
        time.sleep(3)
        status = mongod_control.status()
    LOGGER.info("The mongod process has stopped")

    # We wait a bit, since files could still be flushed to disk, which was causing
    # rsync "file has vanished" errors.
    time.sleep(60)

    return 0


def get_mongo_client_args(host=None, port=None, task_config=None,
                          server_selection_timeout_ms=2 * ONE_HOUR_SECS * 1000,
                          socket_timeout_ms=2 * ONE_HOUR_SECS * 1000, direct_connection=True):
    """Return keyword arg dict used in PyMongo client."""
    # Set the default serverSelectionTimeoutMS & socketTimeoutMS to 10 minutes.
    mongo_args = {
        "serverSelectionTimeoutMS": server_selection_timeout_ms,
        "socketTimeoutMS": socket_timeout_ms,
        "directConnection": direct_connection,
    }
    if host:
        mongo_args["host"] = host
    if port:
        mongo_args["port"] = port
    if task_config:
        # Set the writeConcern
        if task_config.write_concern:
            mongo_args.update(yaml.safe_load(task_config.write_concern))
        # Set the readConcernLevel
        if task_config.read_concern_level:
            mongo_args["readConcernLevel"] = task_config.read_concern_level
    return mongo_args


def mongo_shell(mongo_path, work_dir, host_port, mongo_cmds, retries=5, retry_sleep=5):
    """Start mongo_path from work_dir, connecting to host_port and executes mongo_cmds."""
    cmds = "cd {}; echo {} | {} {}".format(
        pipes.quote(work_dir), pipes.quote(mongo_cmds), pipes.quote(mongo_path), host_port)
    attempt_num = 0
    while True:
        ret, output = execute_cmd(cmds, use_file=True)
        if not ret:
            break
        attempt_num += 1
        if attempt_num > retries:
            break
        time.sleep(retry_sleep)
    return ret, output


def mongod_wait_for_primary(mongo, timeout=60, sleep_interval=3):
    """Return True if mongod primary is available in replica set, within the specified timeout."""

    start = time.time()
    while not mongo.admin.command("isMaster")["ismaster"]:
        time.sleep(sleep_interval)
        if time.time() - start >= timeout:
            return False
    return True


def mongo_reconfig_replication(mongo, host_port, repl_set):
    """Reconfigure the mongod replica set. Return 0 if successful."""

    # TODO: Rework reconfig logic as follows:
    # 1. Start up mongod in standalone
    # 2. Delete the config doc
    # 3. Stop mongod
    # 4. Start mongod
    # When reconfiguring the replica set, due to a switch in ports
    # it can only be done using force=True, as the node will not come up as Primary.
    # The side affect of using force=True are large jumps in the config
    # version, which after many reconfigs may exceed the 'int' value.

    LOGGER.info("Reconfiguring replication %s %s", host_port, repl_set)
    database = pymongo.database.Database(mongo, "local")
    system_replset = database.get_collection("system.replset")
    # Check if replica set has already been initialized
    if system_replset is None or not system_replset.find_one():
        rs_config = {"_id": repl_set, "members": [{"_id": 0, "host": host_port}]}
        ret = mongo.admin.command("replSetInitiate", rs_config)
        LOGGER.info("Replication initialized: %s %s", ret, rs_config)
    else:
        # Wait until replication is initialized.
        while True:
            try:
                ret = mongo.admin.command("replSetGetConfig")
                if ret["ok"] != 1:
                    LOGGER.error("Failed replSetGetConfig: %s", ret)
                    return 1

                rs_config = ret["config"]
                # We only reconfig if there is a change to 'host'.
                if rs_config["members"][0]["host"] != host_port:
                    # With force=True, version is ignored.
                    # rs_config["version"] = rs_config["version"] + 1
                    rs_config["members"][0]["host"] = host_port
                    ret = mongo.admin.command("replSetReconfig", rs_config, force=True)
                    if ret["ok"] != 1:
                        LOGGER.error("Failed replSetReconfig: %s", ret)
                        return 1
                    LOGGER.info("Replication reconfigured: %s", ret)
                break

            except pymongo.errors.AutoReconnect:
                pass
            except pymongo.errors.OperationFailure as err:
                # src/mongo/base/error_codes.yml: error_code("NotYetInitialized", 94)
                if err.code != 94:
                    LOGGER.error("Replication failed to initialize: %s", ret)
                    return 1

    primary_available = mongod_wait_for_primary(mongo)
    LOGGER.debug("isMaster: %s", mongo.admin.command("isMaster"))
    LOGGER.debug("replSetGetStatus: %s", mongo.admin.command("replSetGetStatus"))
    return 0 if ret["ok"] == 1 and primary_available else 1


def mongo_seed_docs(mongo, db_name, coll_name, num_docs):
    """Seed a collection with random document values."""

    def rand_string(max_length=1024):
        """Return random string of random length."""
        return ''.join(
            random.choice(string.ascii_letters) for _ in range(random.randint(1, max_length)))

    LOGGER.info("Seeding DB '%s' collection '%s' with %d documents, %d already exist", db_name,
                coll_name, num_docs, mongo[db_name][coll_name].estimated_document_count())
    random.seed()
    base_num = 100000
    bulk_num = min(num_docs, 10000)
    bulk_loops = num_docs // bulk_num
    for _ in range(bulk_loops):
        num_coll_docs = mongo[db_name][coll_name].estimated_document_count()
        if num_coll_docs >= num_docs:
            break
        mongo[db_name][coll_name].insert_many(
            [{"x": random.randint(0, base_num), "doc": rand_string(1024)} for _ in range(bulk_num)])
    LOGGER.info("After seeding there are %d documents in the collection",
                mongo[db_name][coll_name].estimated_document_count())
    return 0


def mongo_validate_canary(mongo, db_name, coll_name, doc):
    """Validate a canary document, return 0 if the document exists."""
    if not doc:
        return 0
    LOGGER.info("Validating canary document using %s.%s.find_one(%s)", db_name, coll_name, doc)
    return 0 if mongo[db_name][coll_name].find_one(doc) else 1


def mongo_insert_canary(mongo, db_name, coll_name, doc):
    """Insert a canary document with 'j' True, return 0 if successful."""
    LOGGER.info("Inserting canary document using %s.%s.insert_one(%s)", db_name, coll_name, doc)
    coll = mongo[db_name][coll_name].with_options(
        write_concern=pymongo.write_concern.WriteConcern(j=True))
    res = coll.insert_one(doc)
    return 0 if res.inserted_id else 1


def new_resmoke_config(config_file, new_config_file, test_data, eval_str=""):
    """Create 'new_config_file', from 'config_file', with an update from 'test_data'."""
    new_config = {
        "executor": {
            "config": {"shell_options": {"eval": eval_str, "global_vars": {"TestData": test_data}}}
        }
    }
    with open(config_file, "r") as yaml_stream:
        config = yaml.safe_load(yaml_stream)
    config.update(new_config)
    with open(new_config_file, "w") as yaml_stream:
        yaml.safe_dump(config, yaml_stream)


def resmoke_client(work_dir, mongo_path, host_port, js_test, resmoke_suite, repeat_num=1,
                   no_wait=False, log_file=None):
    """Start resmoke client from work_dir, connecting to host_port and executes js_test."""
    log_output = f">> {log_file} 2>&1" if log_file else ""
    cmds = (f"cd {pipes.quote(work_dir)};"
            f" python {powercycle_constants.RESMOKE_PATH}"
            f" run"
            f" --mongo {pipes.quote(mongo_path)}"
            f" --suites {pipes.quote(resmoke_suite)}"
            f" --shellConnString mongodb://{host_port}"
            f" --continueOnFailure"
            f" --repeat {repeat_num}"
            f" {pipes.quote(js_test)}"
            f" {log_output}")
    ret, output = None, None
    if no_wait:
        Processes.create(cmds, use_file=True)
    else:
        ret, output = execute_cmd(cmds, use_file=True)
    return ret, output


def get_remote_python():
    """Return remote python."""

    python_bin_dir = "Scripts" if _IS_WINDOWS else "bin"
    remote_python = f". {powercycle_constants.VIRTUALENV_DIR}/{python_bin_dir}/activate; python -u"

    return remote_python


def main(parser_actions, options):
    """Execute Main program."""

    # pylint: disable=global-statement
    global REPORT_JSON
    global REPORT_JSON_FILE
    global REPORT_JSON_SUCCESS
    global EXIT_YML_FILE
    global EXIT_YML
    # pylint: enable=global-statement

    atexit.register(exit_handler)
    register_signal_handler(dump_stacks_and_exit)

    logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s", level=logging.ERROR,
                        filename=options.log_file)
    logging.getLogger(__name__).setLevel(options.log_level.upper())
    logging.Formatter.converter = time.gmtime

    LOGGER.info("powercycle invocation: %s", " ".join(sys.argv))

    task_name = re.sub(r"(_[0-9]+)(_[\w-]+)?$", "", options.task_name)
    task_config = powercycle_config.get_task_config(task_name, options.remote_operation)

    LOGGER.info("powercycle task config: %s", task_config)

    # Initialize the mongod options
    # Note - We use posixpath for Windows client to Linux server scenarios.
    root_dir = f"{powercycle_constants.REMOTE_DIR}/mongodb-powercycle-test-{int(time.time())}"
    set_fcv_cmd = "set_fcv" if task_config.fcv is not None else ""

    # Error out earlier if these options are not properly specified
    write_concern = yaml.safe_load(task_config.write_concern)

    # Invoke remote_handler if remote_operation is specified.
    # The remote commands are program args.
    if options.remote_operation:
        ret = remote_handler(options, task_config, root_dir)
        # Exit here since the local operations are performed after this.
        local_exit(ret)

    EXIT_YML_FILE = powercycle_constants.POWERCYCLE_EXIT_FILE
    REPORT_JSON_FILE = powercycle_constants.REPORT_JSON_FILE
    REPORT_JSON = {
        "failures":
            0, "results": [{
                "status": "fail", "test_file": task_name, "exit_code": 1, "elapsed": 0,
                "start": int(time.time()), "end": int(time.time())
            }]
    }
    LOGGER.debug("Creating report JSON %s", REPORT_JSON)

    test_loops = task_config.test_loops
    num_crud_clients = powercycle_constants.NUM_CRUD_CLIENTS
    num_fsm_clients = powercycle_constants.NUM_FSM_CLIENTS

    # Windows task overrides:
    #   - Execute no more than 3 test loops
    #   - Cap the maximum number of clients to 10 each
    if _IS_WINDOWS:
        if test_loops > 3:
            test_loops = 3
        num_crud_clients = 10
        num_fsm_clients = 10

    secret_port = powercycle_constants.SECRET_PORT
    standard_port = powercycle_constants.STANDARD_PORT

    seed_docs = "seed_docs"

    rsync_cmd = "rsync_data"
    backup_path_before = powercycle_constants.BACKUP_PATH_BEFORE
    backup_path_after = powercycle_constants.BACKUP_PATH_AFTER
    # Set the first backup directory, for loop 1.
    backup_path_before = f"{backup_path_before}-1"
    backup_path_after = f"{backup_path_after}-1"

    # Setup the mongo client, mongo_path is required if there are local clients.
    mongo_executable = distutils.spawn.find_executable(
        "dist-test/bin/mongo",
        os.getcwd() + os.pathsep + os.environ["PATH"])
    mongo_path = os.path.abspath(os.path.normpath(mongo_executable))

    # Setup the CRUD & FSM clients.
    if not os.path.isfile(powercycle_constants.CONFIG_CRUD_CLIENT):
        LOGGER.error("config crud client %s does not exist",
                     powercycle_constants.CONFIG_CRUD_CLIENT)
        local_exit(1)
    with_external_server = powercycle_constants.CONFIG_CRUD_CLIENT
    fsm_client = powercycle_constants.FSM_CLIENT
    read_concern_level = task_config.read_concern_level
    if write_concern and not read_concern_level:
        read_concern_level = "local"
    crud_test_data = {}
    if read_concern_level:
        crud_test_data["defaultReadConcernLevel"] = read_concern_level
    if write_concern:
        crud_test_data["defaultWriteConcern"] = write_concern
    if read_concern_level or write_concern:
        eval_str = f"load('{powercycle_constants.SET_READ_AND_WRITE_CONCERN}');"
    else:
        eval_str = ""
    fsm_test_data = copy.deepcopy(crud_test_data)
    fsm_test_data["fsmDbDenylist"] = [powercycle_constants.DB_NAME]
    crud_test_data["dbName"] = powercycle_constants.DB_NAME

    # Setup the mongo_repo_root.
    mongo_repo_root_dir = os.getcwd()

    # Setup the validate_canary option.
    canary_doc = ""

    # Set the Pymongo connection timeout to 1 hour for canary insert & validation.
    one_hour_ms = ONE_HOUR_SECS * 1000

    # The remote mongod host comes from the ssh_user_host,
    # which may be specified as user@host.
    ssh_user_host = options.ssh_user_host
    _, ssh_host = get_user_host(ssh_user_host)
    mongod_host = ssh_host

    # As described in http://man7.org/linux/man-pages/man5/ssh_config.5.html, ssh uses the value of
    # the first occurrence for each parameter, so we have the default connection options follow the
    # user-specified --sshConnection options.
    ssh_connection_options = (
        f"{options.ssh_connection_options if options.ssh_connection_options else ''}"
        f" {powercycle_constants.DEFAULT_SSH_CONNECTION_OPTIONS}")
    # For remote operations requiring sudo, force pseudo-tty allocation,
    # see https://stackoverflow.com/questions/10310299/proper-way-to-sudo-over-ssh.
    # Note - the ssh option RequestTTY was added in OpenSSH 5.9, so we use '-tt'.
    ssh_options = "" if _IS_WINDOWS else "-tt"

    # Instantiate the local handler object.
    local_ops = LocalToRemoteOperations(
        user_host=ssh_user_host, ssh_connection_options=ssh_connection_options,
        ssh_options=ssh_options, use_shell=True, access_retry_count=options.ssh_access_retry_count)
    verify_remote_access(local_ops)

    # Pass client_args to the remote script invocation.
    client_args = "powercycle run"
    options_dict = vars(options)
    for action in parser_actions:
        option_value = options_dict.get(action.dest, None)
        if option_value != action.default:
            # The boolean options do not require the option_value.
            if isinstance(option_value, bool):
                option_value = ""
            # Quote the non-default option values from the invocation of this script,
            # if they have spaces, or quotes, such that they can be safely passed to the
            # remote host's invocation of this script.
            elif isinstance(option_value, str) and re.search("\"|'| ", option_value):
                option_value = f"'{option_value}'"
            # The tuple, list or set options need to be changed to a string.
            elif isinstance(option_value, (tuple, list, set)):
                option_value = " ".join(map(str, option_value))
            client_args = f"{client_args} {action.option_strings[-1]} {option_value}"

    script_name = f"{powercycle_constants.REMOTE_DIR}/{powercycle_constants.RESMOKE_PATH}"
    script_name = abs_path(script_name)
    LOGGER.info("%s %s", script_name, client_args)

    remote_python = get_remote_python()

    # Remote install of MongoDB.
    ret, output = call_remote_operation(local_ops, remote_python, script_name, client_args,
                                        "--remoteOperation install_mongod")
    LOGGER.info("****install_mongod: %d %s****", ret, output)
    if ret:
        local_exit(ret)

    loop_num = 0
    start_time = int(time.time())
    test_time = 0

    # ======== Main loop for running the powercycle test========:
    #   1. Rsync the database (optional, post-crash, pre-recovery)
    #   2. Start mongod on the secret port and wait for it to recover
    #   3  Validate collections (optional)
    #   4. Validate canary (optional)
    #   5. Stop mongod
    #   6. Rsync the database (optional, post-recovery)
    #   7. Start mongod on the standard port
    #   8. Start mongo (shell) & FSM clients
    #   9. Generate canary document (optional)
    #  10. Crash the server
    #  11. Exit loop if one of these occurs:
    #      a. Loop time or loop number exceeded
    #      b. Any step fails
    # =========
    while True:
        loop_num += 1
        LOGGER.info("****Starting test loop %d test time %d seconds****", loop_num, test_time)

        temp_client_files = []

        validate_canary_local = False
        if loop_num > 1:
            validate_canary_local = True

        # Since rsync requires Posix style paths, we do not use os.path.join to
        # construct the rsync destination directory.
        new_path_dir = get_backup_path(backup_path_before, loop_num)
        rsync_opt = f"--rsyncDest {backup_path_before} {new_path_dir}"
        backup_path_before = new_path_dir

        # Optionally, rsync the pre-recovery database.
        # Start monogd on the secret port.
        # Optionally validate collections, validate the canary and seed the collection.
        remote_operation = (f"--remoteOperation"
                            f" {rsync_opt}"
                            f" --mongodHost {mongod_host}"
                            f" --mongodPort {secret_port}"
                            f" {rsync_cmd}"
                            f" start_mongod"
                            f" {set_fcv_cmd if loop_num == 1 else ''}"
                            f" {seed_docs if loop_num == 1 else ''}")
        ret, output = call_remote_operation(local_ops, remote_python, script_name, client_args,
                                            remote_operation)
        rsync_text = "rsync_data beforerecovery & "
        LOGGER.info("****%sstart mongod: %d %s****", rsync_text, ret, output)
        if ret:
            local_exit(ret)

        # Optionally validate canary document locally.
        if validate_canary_local:
            mongo = pymongo.MongoClient(**get_mongo_client_args(
                host=mongod_host, port=secret_port, server_selection_timeout_ms=one_hour_ms,
                socket_timeout_ms=one_hour_ms))
            ret = mongo_validate_canary(mongo, powercycle_constants.DB_NAME,
                                        powercycle_constants.COLLECTION_NAME, canary_doc)
            LOGGER.info("Local canary validation: %d", ret)
            if ret:
                local_exit(ret)

        # Run local validation of collections.
        host_port = f"{mongod_host}:{secret_port}"
        new_config_file = NamedTempFile.create(suffix=".yml", directory="tmp")
        temp_client_files.append(new_config_file)
        validation_test_data = {
            "skipValidationOnNamespaceNotFound": True, "allowUncleanShutdowns": True
        }
        new_resmoke_config(with_external_server, new_config_file, validation_test_data)
        ret, output = resmoke_client(mongo_repo_root_dir, mongo_path, host_port,
                                     "jstests/hooks/run_validate_collections.js", new_config_file)
        LOGGER.info("Local collection validation: %d %s", ret, output)
        if ret:
            network_error = (
                f"network error while attempting to run command 'isMaster' on host '{host_port}'")
            # Mark this error as ssh failure, since it happens during the first test loop before
            # the first server crash and likely related to port forwarding not working, which
            # uses ssh tunnel command.
            if loop_num == 1 and network_error in output:
                ssh_failure_exit(ret, network_error)
            local_exit(ret)

        # Shutdown mongod on secret port.
        remote_op = f"--remoteOperation --mongodPort {secret_port} shutdown_mongod"
        ret, output = call_remote_operation(local_ops, remote_python, script_name, client_args,
                                            remote_op)
        LOGGER.info("****shutdown_mongod: %d %s****", ret, output)
        if ret:
            local_exit(ret)

        # Since rsync requires Posix style paths, we do not use os.path.join to
        # construct the rsync destination directory.
        new_path_dir = get_backup_path(backup_path_after, loop_num)
        rsync_opt = f"--rsyncDest {backup_path_after} {new_path_dir}"
        backup_path_after = new_path_dir

        # Optionally, rsync the post-recovery database.
        # Start monogd on the standard port.
        remote_op = (f"--remoteOperation"
                     f" {rsync_opt}"
                     f" --mongodHost {mongod_host}"
                     f" --mongodPort {standard_port}"
                     f" {rsync_cmd}"
                     f" start_mongod")
        ret, output = call_remote_operation(local_ops, remote_python, script_name, client_args,
                                            remote_op)
        rsync_text = "rsync_data afterrecovery & "
        LOGGER.info("****%s start mongod: %d %s****", rsync_text, ret, output)
        if ret:
            local_exit(ret)

        boot_time_after_recovery = get_boot_datetime(output)

        # Start CRUD clients
        host_port = f"{mongod_host}:{standard_port}"
        for i in range(num_crud_clients):
            crud_config_file = NamedTempFile.create(suffix=".yml", directory="tmp")
            crud_test_data["collectionName"] = f"{powercycle_constants.COLLECTION_NAME}-{i}"
            new_resmoke_config(with_external_server, crud_config_file, crud_test_data, eval_str)
            _, _ = resmoke_client(work_dir=mongo_repo_root_dir, mongo_path=mongo_path,
                                  host_port=host_port, js_test=powercycle_constants.CRUD_CLIENT,
                                  resmoke_suite=crud_config_file, repeat_num=100, no_wait=True,
                                  log_file=f"crud_{i}.log")

        LOGGER.info("****Started %d CRUD client(s)****", num_crud_clients)

        # Start FSM clients
        for i in range(num_fsm_clients):
            fsm_config_file = NamedTempFile.create(suffix=".yml", directory="tmp")
            fsm_test_data["dbNamePrefix"] = f"fsm-{i}"
            # Do collection validation only for the first FSM client.
            fsm_test_data["validateCollections"] = bool(i == 0)
            new_resmoke_config(with_external_server, fsm_config_file, fsm_test_data, eval_str)
            _, _ = resmoke_client(work_dir=mongo_repo_root_dir, mongo_path=mongo_path,
                                  host_port=host_port, js_test=fsm_client,
                                  resmoke_suite=fsm_config_file, repeat_num=100, no_wait=True,
                                  log_file=f"fsm_{i}.log")

        LOGGER.info("****Started %d FSM client(s)****", num_fsm_clients)

        # Crash the server. A pre-crash canary document is written to the DB.
        crash_canary = {}
        canary_doc = {"x": time.time()}
        orig_canary_doc = copy.deepcopy(canary_doc)
        mongo = pymongo.MongoClient(**get_mongo_client_args(host=mongod_host, port=standard_port,
                                                            server_selection_timeout_ms=one_hour_ms,
                                                            socket_timeout_ms=one_hour_ms))
        crash_canary["function"] = mongo_insert_canary
        crash_canary["args"] = [
            mongo, powercycle_constants.DB_NAME, powercycle_constants.COLLECTION_NAME, canary_doc
        ]
        ret, output = crash_server_or_kill_mongod(task_config, crash_canary, local_ops, script_name,
                                                  client_args)

        LOGGER.info("Crash server or Kill mongod: %d %s****", ret, output)

        # For internal crashes 'ret' is non-zero, because the ssh session unexpectedly terminates.
        if task_config.crash_method != "internal" and ret:
            raise Exception(f"Crash of server failed: {output}")

        if task_config.crash_method != "kill":
            # Check if the crash failed due to an ssh error.
            if task_config.crash_method == "internal" and local_ops.ssh_error(output):
                ssh_failure_exit(ret, output)
            # Wait a bit after sending command to crash the server to avoid connecting to the
            # server before the actual crash occurs.
            sleep_secs = 120 if _IS_WINDOWS else 30
            time.sleep(sleep_secs)

        # Kill any running clients and cleanup temporary files.
        Processes.kill_all()
        for temp_file in temp_client_files:
            NamedTempFile.delete(temp_file)

        # Reestablish remote access after crash.
        local_ops = LocalToRemoteOperations(user_host=ssh_user_host,
                                            ssh_connection_options=ssh_connection_options,
                                            ssh_options=ssh_options, use_shell=True,
                                            access_retry_count=options.ssh_access_retry_count)
        verify_remote_access(local_ops)
        ret, output = call_remote_operation(local_ops, remote_python, script_name, client_args,
                                            "--remoteOperation noop")
        boot_time_after_crash = get_boot_datetime(output)
        if boot_time_after_crash == -1 or boot_time_after_recovery == -1:
            LOGGER.warning(
                "Cannot compare boot time after recovery: %s with boot time after crash: %s",
                boot_time_after_recovery, boot_time_after_crash)
        elif task_config.crash_method != "kill" and boot_time_after_crash <= boot_time_after_recovery:
            raise Exception(f"System boot time after crash ({boot_time_after_crash}) is not newer"
                            f" than boot time before crash ({boot_time_after_recovery})")

        canary_doc = copy.deepcopy(orig_canary_doc)

        test_time = int(time.time()) - start_time
        LOGGER.info("****Completed test loop %d test time %d seconds****", loop_num, test_time)
        if loop_num == test_loops:
            break

        ret, output = call_remote_operation(local_ops, remote_python, script_name, client_args,
                                            "--remoteOperation check_disk")
        if ret != 0:
            LOGGER.error("****check_disk: %d %s****", ret, output)

    REPORT_JSON_SUCCESS = True
    local_exit(0)
