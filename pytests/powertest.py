#!/usr/bin/env python

"""Powercycle test

Tests robustness of mongod to survive multiple powercycle events.

Client & server side powercycle test script.

This script can be run against any host which is reachable via ssh.
Note - the remote hosts should be running bash shell (this script may fail otherwise).
There are no assumptions on the server what is the current deployment of MongoDB.
For Windows the assumption is that Cygwin is installed.
The server needs these utilities:
    - python 2.7 or higher
    - sshd
    - rsync
This script will either download a MongoDB tarball or use an existing setup.
"""

from __future__ import print_function

import atexit
import collections
import copy
import datetime
import distutils.spawn
import json
import importlib
import logging
import optparse
import os
import pipes
import posixpath
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
import urlparse
import zipfile

import psutil
import pymongo
import requests
import yaml

# The subprocess32 module is untested on Windows and thus isn't recommended for use, even when it's
# installed. See https://github.com/google/python-subprocess32/blob/3.2.7/README.md#usage.
if os.name == "posix" and sys.version_info[0] == 2:
    try:
        import subprocess32 as subprocess
    except ImportError:
        import warnings
        warnings.warn(("Falling back to using the subprocess module because subprocess32 isn't"
                       " available. When using the subprocess module, a child process may"
                       " trigger an invalid free(). See SERVER-22219 for more details."),
                      RuntimeWarning)
        import subprocess
else:
    import subprocess

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# See https://docs.python.org/2/library/sys.html#sys.platform
_IS_WINDOWS = sys.platform == "win32" or sys.platform == "cygwin"
_IS_LINUX = sys.platform.startswith("linux")
_IS_DARWIN = sys.platform == "darwin"


def _try_import(module, name=None):
    """Attempts to import a module and add it as a global variable.
       If the import fails, then this function doesn't trigger an exception."""
    try:
        module_name = module if not name else name
        globals()[module_name] = importlib.import_module(module)
    except ImportError:
        pass


# These modules are used on the 'client' side.
_try_import("buildscripts.aws_ec2", "aws_ec2")
_try_import("buildscripts.remote_operations", "remote_operations")

if _IS_WINDOWS:

    # These modules are used on both sides for dumping python stacks.
    import win32api
    import win32event

    # These modules are used on the 'server' side.
    _try_import("ntsecuritycon")
    _try_import("pywintypes")
    _try_import("win32file")
    _try_import("win32security")
    _try_import("win32service")
    _try_import("win32serviceutil")


__version__ = "1.0"

LOGGER = logging.getLogger(__name__)

REPORT_JSON = {}  # type: ignore
REPORT_JSON_FILE = ""
REPORT_JSON_SUCCESS = False

EXIT_YML = {"exit_code": 0}  # type: ignore
EXIT_YML_FILE = None


def local_exit(code):
    """Capture exit code and invoke sys.exit."""
    EXIT_YML["exit_code"] = code
    sys.exit(code)



def exit_handler():
    """Exit handler actions:
        - Generate report.json
        - Kill spawned processes
        - Delete all named temporary files
    """
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
        except:
            pass

    if EXIT_YML_FILE:
        LOGGER.debug("Exit handler: Saving exit file %s", EXIT_YML_FILE)
        try:
            with open(EXIT_YML_FILE, "w") as yaml_stream:
                yaml.safe_dump(EXIT_YML, yaml_stream)
        except:  # pylint: disable=bare-except
            pass

    LOGGER.debug("Exit handler: Killing processes")
    try:
        Processes.kill_all()
    except:
        pass

    LOGGER.debug("Exit handler: Cleaning up temporary files")
    try:
        NamedTempFile.delete_all()
    except:
        pass


def register_signal_handler(handler):

    def _handle_set_event(event_handle, handler):
        """
        Windows event object handler that will dump the stacks of all threads.
        """
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
            task_timeout_handle = win32event.CreateEvent(
                security_attributes, manual_reset, initial_state, event_name)
        except win32event.error as err:
            LOGGER.error("Exception from win32event.CreateEvent with error: %s", err)
            return

        # Register to close event object handle on exit.
        atexit.register(win32api.CloseHandle, task_timeout_handle)

        # Create thread.
        event_handler_thread = threading.Thread(
            target=_handle_set_event,
            kwargs={"event_handle": task_timeout_handle, "handler": handler},
            name="windows_event_handler_thread")
        event_handler_thread.daemon = True
        event_handler_thread.start()
    else:
        # Otherwise register a signal handler for SIGUSR1.
        signal_num = signal.SIGUSR1
        signal.signal(signal_num, handler)


def dump_stacks_and_exit(signum, frame):
    """
    Handler that will dump the stacks of all threads.
    """
    LOGGER.info("Dumping stacks!")

    sb = []
    frames = sys._current_frames()
    sb.append("Total threads: {}\n".format(len(frames)))
    sb.append("")

    for thread_id in frames:
        stack = frames[thread_id]
        sb.append("Thread {}:".format(thread_id))
        sb.append("".join(traceback.format_stack(stack)))

    LOGGER.info("".join(sb))

    if _IS_WINDOWS:
        exit_handler()
        os._exit(1)
    else:
        sys.exit(1)


def child_processes(parent_pid):
    """Returns a list of all child processes for a pid."""
    # The child processes cannot be obtained from the parent on Windows from psutil. See
    # https://stackoverflow.com/questions/30220732/python-psutil-not-showing-all-child-processes
    child_procs = []
    while psutil.pid_exists(parent_pid):
        try:
            child_procs = [p for p in psutil.process_iter(attrs=["pid"]) if parent_pid == p.ppid()]
            break
        except psutil.NoSuchProcess:
            pass
    for proc in child_procs:
        proc_children = child_processes(proc.pid)
        if proc_children:
            child_procs += proc_children
    return list(set(child_procs))


def kill_process(pid, kill_children=True):
    """Kill a process, and optionally it's children, by it's pid. Returns 0 if successful."""
    try:
        parent = psutil.Process(pid)
    except psutil.NoSuchProcess:
        LOGGER.warn("Could not kill process %d, as it no longer exists", pid)
        return 0

    procs = [parent]
    if kill_children:
        procs += child_processes(pid)

    for proc in procs:
        try:
            LOGGER.debug("Killing process '%s' pid %d", proc.name(), proc.pid)
            proc.kill()
        except psutil.NoSuchProcess:
            LOGGER.warn("Could not kill process %d, as it no longer exists", pid)

    _, alive = psutil.wait_procs(procs, timeout=30, callback=None)
    if alive:
        for proc in alive:
            LOGGER.error("Process %d still alive!", proc.pid)
    return 0


def kill_processes(procs, kill_children=True):
    """Kill a list of processes and optionally it's children."""
    for proc in procs:
        LOGGER.debug("Starting kill of parent process %d", proc.pid)
        kill_process(proc.pid, kill_children=kill_children)
        ret = proc.wait()
        LOGGER.debug("Finished kill of parent process %d has return code of %d", proc.pid, ret)


def get_extension(filename):
    """Returns the extension of a file."""
    return os.path.splitext(filename)[-1]


def executable_extension():
    """Return executable file extension."""
    if _IS_WINDOWS:
        return ".exe"
    return ""


def abs_path(path):
    """Returns absolute path for 'path'. Raises an exception on failure."""
    if _IS_WINDOWS:
        # Get the Windows absolute path.
        cmd = "cygpath -wa {}".format(path)
        ret, output = execute_cmd(cmd, use_file=True)
        if ret:
            raise Exception("Command \"{}\" failed with code {} and output message: {}".format(
                cmd, ret, output))
        return output.rstrip()
    return os.path.abspath(os.path.normpath(path))


def symlink_dir(source_dir, dest_dir):
    """Symlinks the 'dest_dir' to 'source_dir'."""
    if _IS_WINDOWS:
        win32file.CreateSymbolicLink(dest_dir, source_dir, win32file.SYMBOLIC_LINK_FLAG_DIRECTORY)
    else:
        os.symlink(source_dir, dest_dir)


def get_bin_dir(root_dir):
    """Locates the 'bin' directory within 'root_dir' tree."""
    for root, dirs, _ in os.walk(root_dir):
        if "bin" in dirs:
            return os.path.join(root, "bin")
    return None


def create_temp_executable_file(cmds):
    """Creates an executable temporary file containing 'cmds'. Returns file name."""
    temp_file_name = NamedTempFile.create(suffix=".sh", directory="tmp")
    with NamedTempFile.get(temp_file_name) as temp_file:
        temp_file.write(cmds)
    os_st = os.stat(temp_file_name)
    os.chmod(temp_file_name, os_st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return temp_file_name


def start_cmd(cmd, use_file=False):
    """Starts command and returns proc instance from Popen"""

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

    proc = subprocess.Popen(cmd, close_fds=True)
    LOGGER.debug("Spawned process %s pid %d", psutil.Process(proc.pid).name(), proc.pid)

    return proc


def execute_cmd(cmd, use_file=False):
    """Executes command and returns return_code, output from command"""

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

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output, _ = proc.communicate()
        error_code = proc.returncode
        if error_code:
            output = "Error executing cmd {}: {}".format(cmd, output)
    finally:
        if use_file:
            os.remove(temp_file)

    return error_code, output


def get_user_host(user_host):
    """ Returns a tuple (user, host) from the user_host string. """
    if "@" in user_host:
        return tuple(user_host.split("@"))
    return None, user_host


def parse_options(options):
    """ Parses options and returns a dict.

        Since there are options which can be specifed with a short('-') or long
        ('--') form, we preserve that in key map as {option_name: (value, form)}."""
    options_map = collections.defaultdict(list)
    opts = shlex.split(options)
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
    """Returns True if download was successful. Raises error if download fails."""

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
                                    "({} bytes) for URL {}".format(
                                        file_size, url_content_length, url))
                continue

        return True

    raise Exception("Unknown download problem for {} to file {}".format(url, file_name))


def install_tarball(tarball, root_dir):
    """ Unzip and install 'tarball' into 'root_dir'."""

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
    """ Change all file permissions in 'bin_dir' to executable for everyone. """

    files = os.listdir(bin_dir)
    LOGGER.debug("chmod +x %s %s", bin_dir, files)
    for dir_file in files:
        bin_file = os.path.join(bin_dir, dir_file)
        os_st = os.stat(bin_file)
        os.chmod(bin_file, os_st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def chmod_w_file(chmod_file):
    """ Change the permission for 'chmod_file' to '+w' for everyone. """

    LOGGER.debug("chmod +w %s", chmod_file)
    if _IS_WINDOWS:
        # The os package cannot set the directory to '+w', so we use win32security.
        # See https://stackoverflow.com/
        #       questions/12168110/setting-folder-permissions-in-windows-using-python
        user, domain, sec_type = win32security.LookupAccountName("", "Everyone")
        file_sd = win32security.GetFileSecurity(
            chmod_file, win32security.DACL_SECURITY_INFORMATION)
        dacl = file_sd.GetSecurityDescriptorDacl()
        dacl.AddAccessAllowedAce(
            win32security.ACL_REVISION, ntsecuritycon.FILE_GENERIC_WRITE, user)
        file_sd.SetSecurityDescriptorDacl(1, dacl, 0)
        win32security.SetFileSecurity(chmod_file, win32security.DACL_SECURITY_INFORMATION, file_sd)
    else:
        os.chmod(chmod_file, os.stat(chmod_file) | stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)


def set_windows_bootstatuspolicy():
    """ For Windows hosts that are physical, this prevents boot to prompt after failure."""

    LOGGER.info("Setting bootstatuspolicy to ignoreallfailures & boot timeout to 5 seconds")
    cmds = """
        echo 'Setting bootstatuspolicy to ignoreallfailures & boot timeout to 5 seconds' ;
        bcdedit /set {default} bootstatuspolicy ignoreallfailures ;
        bcdedit /set {current} bootstatuspolicy ignoreallfailures ;
        bcdedit /timeout 5"""
    ret, output = execute_cmd(cmds, use_file=True)
    return ret, output


def install_mongod(bin_dir=None, tarball_url="latest", root_dir=None):
    """Sets up 'root_dir'/bin to contain MongoDB binaries.

       If 'bin_dir' is specified, then symlink it to 'root_dir'/bin.
       Otherwise, download 'tarball_url' and symlink it's bin to 'root_dir'/bin.

       If 'bin_dir' is specified, skip download and create symlink
       from 'bin_dir' to 'root_dir'/bin."""

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

    tarball = os.path.split(urlparse.urlsplit(tarball_url).path)[-1]
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
        return datetime.datetime(*map(int, map(float, re.split("[ :-]", match.groups()[0]))))
    return -1


def print_uptime():
    """Prints the last time the system was booted, and the uptime (in seconds). """
    boot_time_epoch = psutil.boot_time()
    boot_time = datetime.datetime.fromtimestamp(boot_time_epoch).strftime('%Y-%m-%d %H:%M:%S.%f')
    uptime = int(time.time() - boot_time_epoch)
    LOGGER.info("System was last booted %s, up %d seconds", boot_time, uptime)


def call_remote_operation(local_ops, remote_python, script_name, client_args, operation):
    """Call the remote operation and return tuple (ret, ouput)."""
    client_call = "{} {} {} {}".format(remote_python, script_name, client_args, operation)
    ret, output = local_ops.shell(client_call)
    return ret, output


def is_instance_running(ret, aws_status):
    """ Return true if instance is in a running state. """
    return ret == 0 and aws_status.state["Name"] == "running"


class Processes(object):
    """Class to create and kill spawned processes."""

    _PROC_LIST = []

    @classmethod
    def create(cls, cmds):
        """Creates a spawned process."""
        proc = start_cmd(cmds, use_file=True)
        cls._PROC_LIST.append(proc)

    @classmethod
    def kill(cls, proc):
        """Kills a spawned process and all it's children."""
        kill_processes([proc], kill_children=True)
        cls._PROC_LIST.remove(proc)

    @classmethod
    def kill_all(cls):
        """Kill all spawned processes."""
        procs = copy.copy(cls._PROC_LIST)
        for proc in procs:
            cls.kill(proc)


class NamedTempFile(object):
    """Class to control temporary files."""

    _FILE_MAP = {}
    _DIR_LIST = []

    @classmethod
    def create(cls, directory=None, suffix=""):
        """Creates a temporary file, and optional directory, and returns the file name."""
        if directory and not os.path.isdir(directory):
            LOGGER.debug("Creating temporary directory %s", directory)
            os.makedirs(directory)
            cls._DIR_LIST.append(directory)
        temp_file = tempfile.NamedTemporaryFile(suffix=suffix, dir=directory, delete=False)
        cls._FILE_MAP[temp_file.name] = temp_file
        return temp_file.name

    @classmethod
    def get(cls, name):
        """Gets temporary file object.  Raises an exception if the file is unknown."""
        if name not in cls._FILE_MAP:
            raise Exception("Unknown temporary file {}.".format(name))
        return cls._FILE_MAP[name]

    @classmethod
    def delete(cls, name):
        """Deletes temporary file. Raises an exception if the file is unknown."""
        if name not in cls._FILE_MAP:
            raise Exception("Unknown temporary file {}.".format(name))
        if not os.path.exists(name):
            LOGGER.debug("Temporary file %s no longer exists", name)
            del cls._FILE_MAP[name]
            return
        try:
            os.remove(name)
        except (IOError, OSError) as err:
            LOGGER.warn("Unable to delete temporary file %s with error %s", name, err)
        if not os.path.exists(name):
            del cls._FILE_MAP[name]

    @classmethod
    def delete_dir(cls, directory):
        """Deletes temporary directory. Raises an exception if the directory is unknown."""
        if directory not in cls._DIR_LIST:
            raise Exception("Unknown temporary directory {}.".format(directory))
        if not os.path.exists(directory):
            LOGGER.debug("Temporary directory %s no longer exists", directory)
            cls._DIR_LIST.remove(directory)
            return
        try:
            shutil.rmtree(directory)
        except (IOError, OSError) as err:
            LOGGER.warn(
                "Unable to delete temporary directory %s with error %s", directory, err)
        if not os.path.exists(directory):
            cls._DIR_LIST.remove(directory)

    @classmethod
    def delete_all(cls):
        """Deletes all temporary files and directories."""
        for name in list(cls._FILE_MAP):
            cls.delete(name)
        for directory in cls._DIR_LIST:
            cls.delete_dir(directory)


class ProcessControl(object):
    """ Process control class.

        Control processes either by name or a list of pids. If name is supplied, then
        all matching pids are controlled."""

    def __init__(self, name=None, pids=None):
        """Provide either 'name' or 'pids' to control the process."""
        if not name and not pids:
            raise Exception("Either 'process_name' or 'pids' must be specifed")
        self.name = name
        self.pids = []
        if pids:
            self.pids = pids
        self.procs = []

    def get_pids(self):
        """ Returns list of process ids for process 'self.name'."""
        if not self.name:
            return self.pids
        self.pids = []
        for proc in psutil.process_iter():
            if proc.name() == self.name:
                self.pids.append(proc.pid)
        return self.pids

    def get_name(self):
        """ Returns process name or name of first running process from pids."""
        if not self.name:
            for pid in self.get_pids():
                proc = psutil.Process(pid)
                if psutil.pid_exists(pid):
                    self.name = proc.name()
                    break
        return self.name

    def get_procs(self):
        """ Returns a list of 'proc' for the associated pids."""
        procs = []
        for pid in self.get_pids():
            procs.append(psutil.Process(pid))
        return procs

    def is_running(self):
        """ Returns true if any process is running that either matches on name or pids."""
        for pid in self.get_pids():
            if psutil.pid_exists(pid):
                return True
        return False

    def kill(self):
        """ Kills all running processes that match the list of pids. """
        if self.is_running():
            for proc in self.get_procs():
                try:
                    proc.kill()
                except psutil.NoSuchProcess:
                    LOGGER.info("Could not kill process with pid %d, as it no longer exists",
                                proc.pid)


class WindowsService(object):
    """ Windows service control class."""

    def __init__(self,
                 name,
                 bin_path,
                 bin_options,
                 start_type=None):

        self.name = name
        self.bin_name = os.path.basename(bin_path)
        self.bin_path = bin_path
        self.bin_options = bin_options
        if start_type is not None:
            self.start_type = start_type
        else:
            self.start_type = win32service.SERVICE_DEMAND_START
        self.pids = []
        self._states = {
            win32service.SERVICE_CONTINUE_PENDING: "continue pending",
            win32service.SERVICE_PAUSE_PENDING: "pause pending",
            win32service.SERVICE_PAUSED: "paused",
            win32service.SERVICE_RUNNING: "running",
            win32service.SERVICE_START_PENDING: "start pending",
            win32service.SERVICE_STOPPED: "stopped",
            win32service.SERVICE_STOP_PENDING: "stop pending",
            }

    def create(self):
        """ Create service, if not installed. Returns (code, output) tuple. """
        if self.status() in self._states.values():
            return 1, "Service '{}' already installed, status: {}".format(self.name, self.status())
        try:
            win32serviceutil.InstallService(
                pythonClassString="Service.{}".format(self.name),
                serviceName=self.name,
                displayName=self.name,
                startType=self.start_type,
                exeName=self.bin_path,
                exeArgs=self.bin_options)
            ret = 0
            output = "Service '{}' created".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = "{}: {}".format(err[1], err[2])

        return ret, output

    def update(self):
        """ Update installed service. Returns (code, output) tuple. """
        if self.status() not in self._states.values():
            return 1, "Service update '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.ChangeServiceConfig(
                pythonClassString="Service.{}".format(self.name),
                serviceName=self.name,
                displayName=self.name,
                startType=self.start_type,
                exeName=self.bin_path,
                exeArgs=self.bin_options)
            ret = 0
            output = "Service '{}' updated".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = "{}: {}".format(err[1], err[2])

        return ret, output

    def delete(self):
        """ Delete service. Returns (code, output) tuple. """
        if self.status() not in self._states.values():
            return 1, "Service delete '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.RemoveService(serviceName=self.name)
            ret = 0
            output = "Service '{}' deleted".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = "{}: {}".format(err[1], err[2])

        return ret, output

    def start(self):
        """ Start service. Returns (code, output) tuple. """
        if self.status() not in self._states.values():
            return 1, "Service start '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.StartService(serviceName=self.name)
            ret = 0
            output = "Service '{}' started".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = "{}: {}".format(err[1], err[2])

        proc = ProcessControl(name=self.bin_name)
        self.pids = proc.get_pids()

        return ret, output

    def stop(self, timeout):
        """Stop service, waiting for 'timeout' seconds. Return (code, output) tuple."""
        self.pids = []
        if self.status() not in self._states.values():
            return 1, "Service '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.StopService(serviceName=self.name)
            start = time.time()
            status = self.status()
            while status == "stop pending":
                if time.time() - start >= timeout:
                    ret = 1
                    output = "Service '{}' status is '{}'".format(self.name, status)
                    break
                time.sleep(3)
                status = self.status()
            ret = 0
            output = "Service '{}' stopped".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = "{}: {}".format(err[1], err[2])

        return ret, output

    def status(self):
        """ Returns state of the service as a string. """
        try:
            # QueryServiceStatus returns a tuple:
            #   (scvType, svcState, svcControls, err, svcErr, svcCP, svcWH)
            # See https://msdn.microsoft.com/en-us/library/windows/desktop/ms685996(v=vs.85).aspx
            scv_type, svc_state, svc_controls, err, svc_err, svc_cp, svc_wh = (
                win32serviceutil.QueryServiceStatus(serviceName=self.name))
            if svc_state in self._states:
                return self._states[svc_state]
            return "unknown"
        except pywintypes.error:
            return "not installed"

    def get_pids(self):
        """ Return list of pids for service. """
        return self.pids


class PosixService(object):
    """ Service control on POSIX systems.

        Simulates service control for background processes which fork themselves,
        i.e., mongod with '--fork'."""

    def __init__(self, name, bin_path, bin_options):
        self.name = name
        self.bin_path = bin_path
        self.bin_name = os.path.basename(bin_path)
        self.bin_options = bin_options
        self.pids = []

    def create(self):
        """ Simulates create service. Returns (code, output) tuple. """
        return 0, None

    def update(self):
        """ Simulates update service. Returns (code, output) tuple. """
        return 0, None

    def delete(self):
        """ Simulates delete service. Returns (code, output) tuple. """
        return 0, None

    def start(self):
        """ Start process. Returns (code, output) tuple. """
        cmd = "{} {}".format(self.bin_path, self.bin_options)
        ret, output = execute_cmd(cmd)
        if not ret:
            proc = ProcessControl(name=self.bin_name)
            self.pids = proc.get_pids()
        return ret, output

    def stop(self, timeout):  # pylint: disable=unused-argument
        """Stop process. Returns (code, output) tuple."""
        proc = ProcessControl(name=self.bin_name)
        proc.kill()
        self.pids = []
        return 0, None

    def status(self):
        """ Returns status of service. """
        if self.get_pids():
            return "running"
        return "stopped"

    def get_pids(self):
        """ Return list of pids for process. """
        return self.pids


class MongodControl(object):
    """ Control mongod process. """

    def __init__(  # pylint: disable=too-many-arguments
            self, bin_dir, db_path, log_path, port, options=None):
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
            self.service = self._service("mongod-powertest", self.bin_path, self.mongod_options())

    def set_mongod_option(self, option, option_value=None, option_form="--"):
        """ Sets mongod command line option. """
        self.options_map[option] = (option_value, option_form)

    def get_mongod_option(self, option):
        """ Returns tuple of (value, form). """
        return self.options_map[option]

    def get_mongod_service(self):
        """ Returns the service object used to control mongod. """
        return self.service

    def mongod_options(self):
        """ Returns string of mongod options, which can be used when invoking mongod. """
        opt_string = ""
        for opt_name in self.options_map:
            opt_val, opt_form = self.options_map[opt_name]
            opt_string += " {}{}".format(opt_form, opt_name)
            if opt_val:
                opt_string += " {}".format(opt_val)
        return opt_string

    def install(self, root_dir, tarball_url):
        """ Returns tuple (ret, ouput). """
        # Install mongod, if 'root_dir' does not exist.
        if os.path.isdir(root_dir):
            LOGGER.warning("Root dir %s already exists", root_dir)
        else:
            install_mongod(bin_dir=self.bin_dir, tarball_url=tarball_url, root_dir=root_dir)
        self.bin_dir = get_bin_dir(root_dir)
        if not self.bin_dir:
            ret, output = execute_cmd("ls -lR '{}'".format(root_dir), use_file=True)
            LOGGER.debug(output)
            return 1, "No bin dir can be found under {}".format(root_dir)
        self.bin_path = os.path.join(self.bin_dir, self.process_name)
        # We need to instantiate the Service when installing, since the bin_path
        # is only known after install_mongod runs.
        self.service = self._service("mongod-powertest", self.bin_path, self.mongod_options())
        ret, output = self.service.create()
        return ret, output

    def uninstall(self):
        """ Returns tuple (ret, ouput). """
        return self.service.delete()

    def cleanup(self, root_dir):
        """ Returns tuple (ret, ouput). """
        shutil.rmtree(root_dir, ignore_errors=True)
        return 0, None

    def start(self):
        """ Returns tuple (ret, ouput). """
        return self.service.start()

    def update(self):
        """ Returns tuple (ret, ouput). """
        return self.service.update()

    def stop(self, timeout=0):
        """Return tuple (ret, ouput)."""
        return self.service.stop(timeout)

    def status(self):
        """Return status of the process."""
        return self.service.status()

    def get_pids(self):
        """ Return list of pids for process. """
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

    def __init__(  # pylint: disable=too-many-arguments
            self, user_host, retries=2, retry_sleep=30, ssh_connection_options=None,
            ssh_options=None, shell_binary="/bin/bash", use_shell=False):
        """Initialize LocalToRemoteOperations."""

        self.remote_op = remote_operations.RemoteOperations(  # pylint: disable=undefined-variable
            user_host=user_host, ssh_connection_options=ssh_connection_options,
            ssh_options=ssh_options, retries=retries, retry_sleep=retry_sleep, debug=True,
            shell_binary=shell_binary, use_shell=use_shell)

    def shell(self, cmds, remote_dir=None):
        """ Returns tuple (ret, output) from performing remote shell operation. """
        return self.remote_op.shell(cmds, remote_dir)

    def copy_from(self, files, remote_dir=None):
        """ Returns tuple (ret, output) from performing remote copy_to operation. """
        return self.remote_op.copy_from(files, remote_dir)

    def copy_to(self, files, remote_dir=None):
        """ Returns tuple (ret, output) from performing remote copy_from operation. """
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


def remote_handler(options, operations):
    """ Remote operations handler executes all remote operations on the remote host.

        These operations are invoked on the remote host's copy of this script.
        Only one operation can be performed at a time. """

    # Set 'root_dir' to absolute path.
    root_dir = abs_path(options.root_dir)
    if not operations:
        raise ValueError("No remote operation specified.")

    print_uptime()
    LOGGER.info("Operations to perform %s", operations)
    host = options.host if options.host else "localhost"
    host_port = "{}:{}".format(host, options.port)

    if options.repl_set:
        options.mongod_options = "{} --replSet {}".format(options.mongod_options, options.repl_set)

    # For MongodControl, the file references should be fully specified.
    if options.mongodb_bin_dir:
        bin_dir = abs_path(options.mongodb_bin_dir)
    else:
        bin_dir = get_bin_dir(root_dir)
    db_path = abs_path(options.db_path)
    log_path = abs_path(options.log_path)

    mongod = MongodControl(
        bin_dir=bin_dir,
        db_path=db_path,
        log_path=log_path,
        port=options.port,
        options=options.mongod_options)

    mongo_client_opts = get_mongo_client_args(host=host, port=options.port, options=options)

    # Perform the sequence of operations specified. If any operation fails then return immediately.
    for operation in operations:
        ret = 0
        if operation == "noop":
            pass

        # This is the internal "crash" mechanism, which is executed on the remote host.
        elif operation == "crash_server":
            ret, output = internal_crash(options.remote_sudo, options.crash_option)
            # An internal crash on Windows is not immediate
            try:
                LOGGER.info("Waiting after issuing internal crash!")
                time.sleep(30)
            except IOError:
                pass

        elif operation == "kill_mongod":
            # Unconditional kill of mongod.
            ret, output = kill_mongod()
            if ret:
                LOGGER.error("kill_mongod failed %s", output)
                return ret
            # Ensure the mongod service is not in a running state.
            mongod.stop(timeout=30)
            status = mongod.status()
            if status != "stopped":
                LOGGER.error("Unable to stop the mongod service, in state '%s'", status)
                ret = 1

        elif operation == "install_mongod":
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

        elif operation == "start_mongod":
            # Always update the service before starting, as options might have changed.
            ret, output = mongod.update()
            LOGGER.info(output)
            ret, output = mongod.start()
            LOGGER.info(output)
            if ret:
                LOGGER.error("Failed to start mongod on port %d: %s", options.port, output)
                return ret
            LOGGER.info("Started mongod running on port %d pid %s",
                        options.port, mongod.get_pids())
            mongo = pymongo.MongoClient(**mongo_client_opts)
            LOGGER.info("Server buildinfo: %s", mongo.admin.command("buildinfo"))
            LOGGER.info("Server serverStatus: %s", mongo.admin.command("serverStatus"))
            if options.repl_set:
                ret = mongo_reconfig_replication(mongo, host_port, options.repl_set)

        elif operation == "stop_mongod":
            ret, output = mongod.stop()
            LOGGER.info(output)
            ret = wait_for_mongod_shutdown(mongod)

        elif operation == "shutdown_mongod":
            mongo = pymongo.MongoClient(**mongo_client_opts)
            try:
                mongo.admin.command("shutdown", force=True)
            except pymongo.errors.AutoReconnect:
                pass
            ret = wait_for_mongod_shutdown(mongod)

        elif operation == "rsync_data":
            rsync_dir, new_rsync_dir = options.rsync_dest
            ret, output = rsync(options.db_path, rsync_dir, options.rsync_exclude_files)
            if output:
                LOGGER.info(output)
            # Rename the rsync_dir only if it has a different name than new_rsync_dir.
            if ret == 0 and rsync_dir != new_rsync_dir:
                LOGGER.info("Renaming directory %s to %s", rsync_dir, new_rsync_dir)
                os.rename(rsync_dir, new_rsync_dir)

        elif operation == "seed_docs":
            mongo = pymongo.MongoClient(**mongo_client_opts)
            ret = mongo_seed_docs(
                mongo, options.db_name, options.collection_name, options.seed_doc_num)

        elif operation == "validate_collections":
            mongo = pymongo.MongoClient(**mongo_client_opts)
            ret = mongo_validate_collections(mongo)

        elif operation == "insert_canary":
            mongo = pymongo.MongoClient(**mongo_client_opts)
            ret = mongo_insert_canary(
                mongo, options.db_name, options.collection_name, options.canary_doc)

        elif operation == "validate_canary":
            mongo = pymongo.MongoClient(**mongo_client_opts)
            ret = mongo_validate_canary(
                mongo, options.db_name, options.collection_name, options.canary_doc)

        elif operation == "set_fcv":
            mongo = pymongo.MongoClient(**mongo_client_opts)
            try:
                ret = mongo.admin.command("setFeatureCompatibilityVersion", options.fcv_version)
                ret = 0 if ret["ok"] == 1 else 1
            except pymongo.errors.OperationFailure as err:
                LOGGER.error(err.message)
                ret = err.code

        elif operation == "remove_lock_file":
            lock_file = os.path.join(options.db_path, "mongod.lock")
            if os.path.exists(lock_file):
                LOGGER.debug("Deleting mongod lockfile %s", lock_file)
                try:
                    os.remove(lock_file)
                except (IOError, OSError) as err:
                    LOGGER.warn(
                        "Unable to delete mongod lockfile %s with error %s", lock_file, err)
                    ret = err.code

        else:
            LOGGER.error("Unsupported remote option specified '%s'", operation)
            ret = 1

        if ret:
            return ret

    return 0


def get_backup_path(path, loop_num):
    """Return the backup path based on the loop_num."""
    return re.sub("-{}$".format(loop_num - 1), "-{}".format(loop_num), path)


def rsync(src_dir, dest_dir, exclude_files=None):
    """ Rsync 'src_dir' to 'dest_dir'. """
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
    cmds = "rsync -va --delete --quiet {} {} {}".format(exclude_options, src_dir, dest_dir)
    ret, output = execute_cmd(cmds)
    return ret, output


def kill_mongod():
    """Kill all mongod processes uncondtionally."""
    if _IS_WINDOWS:
        cmds = "taskkill /f /im mongod.exe"
    else:
        cmds = "pkill -9 mongod"
    ret, output = execute_cmd(cmds, use_file=True)
    return ret, output


def internal_crash(use_sudo=False, crash_option=None):
    """ Internally crash the host this excutes on. """

    # Windows can use NotMyFault to immediately crash itself, if it's been installed.
    # See https://docs.microsoft.com/en-us/sysinternals/downloads/notmyfault
    # Otherwise it's better to use an external mechanism instead.
    if _IS_WINDOWS:
        cmds = crash_option if crash_option else "shutdown /r /f /t 0"
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
        sudo = "/usr/bin/sudo" if use_sudo else ""
        cmds = """
            echo "Server crashing now" | {sudo} wall ;
            echo 1 | {sudo} tee /proc/sys/kernel/sysrq ;
            echo b | {sudo} tee /proc/sysrq-trigger""".format(sudo=sudo)
        ret, output = execute_cmd(cmds, use_file=True)
    LOGGER.debug(output)
    return 1, "Crash did not occur"


def crash_server_or_kill_mongod(  # pylint: disable=too-many-arguments,,too-many-locals
        options, crash_canary, canary_port, local_ops, script_name, client_args):
    """Crash server or kill mongod and optionally write canary doc. Return tuple (ret, output)."""

    crash_wait_time = options.crash_wait_time + random.randint(0, options.crash_wait_time_jitter)
    message_prefix = "Killing mongod" if options.crash_method == "kill" else "Crashing server"
    LOGGER.info("%s in %d seconds", message_prefix, crash_wait_time)
    time.sleep(crash_wait_time)

    if options.crash_method == "mpower":
        # Provide time for power to dissipate by sleeping 10 seconds before turning it back on.
        crash_func = local_ops.shell
        crash_args = ["""
            echo 0 > /dev/{crash_option} ;
            sleep 10 ;
            echo 1 > /dev/{crash_option}""".format(crash_option=options.crash_option)
        ]
        local_ops = LocalToRemoteOperations(user_host=options.ssh_crash_user_host,
                                            ssh_connection_options=options.ssh_crash_option,
                                            shell_binary="/bin/sh")
        verify_remote_access(local_ops)

    elif options.crash_method == "internal" or options.crash_method == "kill":
        crash_cmd = "crash_server" if options.crash_method == "internal" else "kill_mongod"
        if options.canary == "remote":
            # The crash canary function executes remotely, only if the
            # crash_method is 'internal'.
            canary = "--mongodPort {} --docForCanary \"{}\"".format(
                canary_port, crash_canary["args"][3])
            canary_cmd = "insert_canary"
        else:
            canary = ""
            canary_cmd = ""
        crash_func = local_ops.shell
        crash_args = ["{} {} --remoteOperation {} {} {} {}".format(
            options.remote_python,
            script_name,
            client_args,
            canary,
            canary_cmd,
            crash_cmd)]

    elif options.crash_method == "aws_ec2":
        ec2 = aws_ec2.AwsEc2()
        crash_func = ec2.control_instance
        crash_args = ["force-stop", options.instance_id, 600, True]

    else:
        message = "Unsupported crash method '{}' provided".format(options.crash_method)
        LOGGER.error(message)
        return 1, message

    # Invoke the crash canary function, right before crashing the server.
    if crash_canary and options.canary == "local":
        crash_canary["function"](*crash_canary["args"])
    ret, output = crash_func(*crash_args)
    LOGGER.info(output)
    return ret, output


def wait_for_mongod_shutdown(mongod_control, timeout=120):
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
    time.sleep(5)

    return 0


def get_mongo_client_args(host=None,
                          port=None,
                          options=None,
                          server_selection_timeout_ms=600000,
                          socket_timeout_ms=600000):
    """ Returns keyword arg dict used in PyMongo client. """
    # Set the default serverSelectionTimeoutMS & socketTimeoutMS to 10 minutes.
    mongo_args = {
        "serverSelectionTimeoutMS": server_selection_timeout_ms,
        "socketTimeoutMS": socket_timeout_ms
    }
    if host:
        mongo_args["host"] = host
    if port:
        mongo_args["port"] = port
    # Set the writeConcern
    if hasattr(options, "write_concern"):
        mongo_args.update(yaml.safe_load(options.write_concern))
    # Set the readConcernLevel
    if hasattr(options, "read_concern_level") and options.read_concern_level:
        mongo_args["readConcernLevel"] = options.read_concern_level
    return mongo_args


def mongo_shell(mongo_path, work_dir, host_port, mongo_cmds, retries=5, retry_sleep=5):
    """Starts mongo_path from work_dir, connecting to host_port and executes mongo_cmds."""
    cmds = ("""
            cd {};
            echo {} | {} {}""".format(
                pipes.quote(work_dir),
                pipes.quote(mongo_cmds),
                pipes.quote(mongo_path),
                host_port))
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
    """ Return True if the mongod primary is available in replica set,
        within the specified timeout."""

    start = time.time()
    while not mongo.admin.command("isMaster")["ismaster"]:
        time.sleep(sleep_interval)
        if time.time() - start >= timeout:
            return False
    return True


def mongo_reconfig_replication(mongo, host_port, repl_set):
    """ Reconfigure the mongod replica set. Return 0 if successful."""

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
    if not system_replset or not system_replset.find_one():
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
                # src/mongo/base/error_codes.err: error_code("NotYetInitialized", 94)
                if err.code != 94:
                    LOGGER.error("Replication failed to initialize: %s", ret)
                    return 1

    primary_available = mongod_wait_for_primary(mongo)
    LOGGER.debug("isMaster: %s", mongo.admin.command("isMaster"))
    LOGGER.debug("replSetGetStatus: %s", mongo.admin.command("replSetGetStatus"))
    return 0 if ret["ok"] == 1 and primary_available else 1


def mongo_seed_docs(mongo, db_name, coll_name, num_docs):
    """ Seed a collection with random document values. """

    def rand_string(max_length=1024):
        """Returns random string of random length. """
        return ''.join(random.choice(string.letters) for _ in range(random.randint(1, max_length)))

    LOGGER.info("Seeding DB '%s' collection '%s' with %d documents, %d already exist",
                db_name, coll_name, num_docs, mongo[db_name][coll_name].count())
    random.seed()
    base_num = 100000
    bulk_num = min(num_docs, 10000)
    bulk_loops = num_docs / bulk_num
    for _ in xrange(bulk_loops):
        num_coll_docs = mongo[db_name][coll_name].count()
        if num_coll_docs >= num_docs:
            break
        mongo[db_name][coll_name].insert_many(
            [{"x": random.randint(0, base_num), "doc": rand_string(1024)}
             for _ in xrange(bulk_num)])
    LOGGER.info("After seeding there are %d documents in the collection",
                mongo[db_name][coll_name].count())
    return 0


def mongo_validate_collections(mongo):
    """ Validates the mongo collections. Returns 0 if all are valid. """

    LOGGER.info("Validating all collections")
    invalid_colls = []
    ebusy_colls = []
    for db_name in mongo.database_names():
        for coll in mongo[db_name].list_collections(filter={"type": "collection"}):
            coll_name = coll["name"]
            res = mongo[db_name].command({"validate": coll_name, "full": True})
            LOGGER.info("Validating %s %s: %s", db_name, coll_name, res)
            ebusy = "EBUSY" in res["errors"] or "EBUSY" in res["warnings"]
            if not res["valid"]:
                invalid_colls.append(coll_name)
            elif ebusy:
                ebusy_colls.append(coll_name)
    if ebusy_colls:
        LOGGER.warning("EBUSY collections: %s", ebusy_colls)
    if invalid_colls:
        LOGGER.error("Invalid collections: %s", ebusy_colls)

    return 0 if not invalid_colls else 1


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
    """ Creates 'new_config_file', from 'config_file', with an update from 'test_data'. """
    new_config = {
        "executor": {
            "config": {
                "shell_options": {
                    "eval": eval_str,
                    "global_vars": {
                        "TestData": test_data
                    }
                }
            }
        }
    }
    with open(config_file, "r") as yaml_stream:
        config = yaml.load(yaml_stream)
    config.update(new_config)
    with open(new_config_file, "w") as yaml_stream:
        yaml.safe_dump(config, yaml_stream)


def resmoke_client(work_dir,
                   mongo_path,
                   host_port,
                   js_test,
                   resmoke_suite,
                   repeat_num=1,
                   no_wait=False,
                   log_file=None):
    """Starts resmoke client from work_dir, connecting to host_port and executes js_test."""
    log_output = ">> {} 2>&1".format(log_file) if log_file else ""
    cmds = ("cd {}; "
            "python buildscripts/resmoke.py"
            " --mongo {}"
            " --suites {}"
            " --shellConnString mongodb://{}"
            " --continueOnFailure"
            " --repeat {}"
            " {}"
            " {}".format(
                pipes.quote(work_dir),
                pipes.quote(mongo_path),
                pipes.quote(resmoke_suite),
                host_port,
                repeat_num,
                pipes.quote(js_test),
                log_output))
    ret, output = None, None
    if no_wait:
        Processes.create(cmds)
    else:
        ret, output = execute_cmd(cmds, use_file=True)
    return ret, output


def main():
    """ Main program. """

    # pylint: disable=global-statement
    global REPORT_JSON
    global REPORT_JSON_FILE
    global REPORT_JSON_SUCCESS
    global EXIT_YML_FILE
    global EXIT_YML
    # pylint: enable=global-statement

    atexit.register(exit_handler)
    register_signal_handler(dump_stacks_and_exit)

    parser = optparse.OptionParser(usage="""
%prog [options]

MongoDB Powercycle test

Examples:

    Server is running as single node replica set connected to mFi mPower, outlet1:
      python powertest.py
            --sshUserHost 10.4.1.54
            --rootDir pt-mmap
            --replSet power
            --crashMethod mpower
            --crashOption output1
            --sshCrashUserHost admin@10.4.100.2
            --sshCrashOption "-oKexAlgorithms=+diffie-hellman-group1-sha1 -i /Users/jonathan/.ssh/mFi.pem"
            --mongodOptions "--storageEngine mmapv1"

    Linux server running in AWS, testing nojournal:
      python powertest.py
            --sshUserHost ec2-user@52.4.173.196
            --sshConnection "-i $HOME/.ssh/JAkey.pem"
            --rootDir pt-nojournal
            --mongodOptions "--nojournal"
""")

    test_options = optparse.OptionGroup(parser, "Test Options")
    crash_options = optparse.OptionGroup(parser, "Crash Options")
    mongodb_options = optparse.OptionGroup(parser, "MongoDB Options")
    mongod_options = optparse.OptionGroup(parser, "mongod Options")
    client_options = optparse.OptionGroup(parser, "Client Options")
    program_options = optparse.OptionGroup(parser, "Program Options")

    # Test options
    test_options.add_option("--sshUserHost", dest="ssh_user_host",
                            help="Server ssh user/host, i.e., user@host (REQUIRED)", default=None)

    default_ssh_connection_options = ("-o ServerAliveCountMax=10"
                                      " -o ServerAliveInterval=6"
                                      " -o StrictHostKeyChecking=no"
                                      " -o ConnectTimeout=10"
                                      " -o ConnectionAttempts=20")
    test_options.add_option("--sshConnection", dest="ssh_connection_options",
                            help="Server ssh additional connection options, i.e., '-i ident.pem'"
                                 " which are added to '{}'".format(default_ssh_connection_options),
                            default=None)

    test_options.add_option("--testLoops",
                            dest="num_loops",
                            help="Number of powercycle loops to run [default: %default]",
                            type="int",
                            default=10)

    test_options.add_option("--testTime",
                            dest="test_time",
                            help="Time to run test (in seconds), overrides --testLoops",
                            type="int",
                            default=0)

    test_options.add_option("--rsync",
                            dest="rsync_data",
                            help="Rsync data directory between mongod stop and start",
                            action="store_true",
                            default=False)

    test_options.add_option("--rsyncExcludeFiles",
                            dest="rsync_exclude_files",
                            help="Files excluded from rsync of the data directory",
                            action="append",
                            default=None)

    test_options.add_option("--backupPathBefore",
                            dest="backup_path_before",
                            help="Path where the db_path is backed up before crash recovery,"
                                 " defaults to '<rootDir>/data-beforerecovery'",
                            default=None)

    test_options.add_option("--backupPathAfter",
                            dest="backup_path_after",
                            help="Path where the db_path is backed up after crash recovery,"
                                 " defaults to '<rootDir>/data-afterrecovery'",
                            default=None)

    validate_locations = ["local", "remote"]
    test_options.add_option("--validate",
                            dest="validate_collections",
                            help="Run validate on all collections after mongod restart after"
                                 " a powercycle. Choose from {} to specify where the"
                                 " validate runs.".format(validate_locations),
                            choices=validate_locations,
                            default=None)

    canary_locations = ["local", "remote"]
    test_options.add_option("--canary",
                            dest="canary",
                            help="Generate and validate canary document between powercycle"
                                 " events. Choose from {} to specify where the canary is"
                                 " generated from. If the 'crashMethod' is not 'internal"
                                 " then this option must be 'local'.".format(canary_locations),
                            choices=canary_locations,
                            default=None)

    test_options.add_option("--docForCanary",
                            dest="canary_doc",
                            help=optparse.SUPPRESS_HELP,
                            default="")

    test_options.add_option("--seedDocNum",
                            dest="seed_doc_num",
                            help="Number of documents to seed the default collection [default:"
                                 " %default]",
                            type="int",
                            default=0)

    test_options.add_option("--dbName",
                            dest="db_name",
                            help=optparse.SUPPRESS_HELP,
                            default="power")

    test_options.add_option("--collectionName",
                            dest="collection_name",
                            help=optparse.SUPPRESS_HELP,
                            default="cycle")

    test_options.add_option("--writeConcern",
                            dest="write_concern",
                            help="mongo (shell) CRUD client writeConcern, i.e.,"
                                 " '{\"w\": \"majority\"}' [default: '%default']",
                            default="{}")

    test_options.add_option("--readConcernLevel",
                            dest="read_concern_level",
                            help="mongo (shell) CRUD client readConcernLevel, i.e.,"
                                 "'majority'",
                            default=None)

    # Crash options
    crash_methods = ["aws_ec2", "internal", "kill", "mpower"]
    crash_options.add_option("--crashMethod",
                             dest="crash_method",
                             choices=crash_methods,
                             help="Crash methods: {} [default: '%default']."
                                  " Select 'aws_ec2' to force-stop/start an AWS instance."
                                  " Select 'internal' to crash the remote server through an"
                                  " internal command, i.e., sys boot (Linux) or notmyfault"
                                  " (Windows). Select 'kill' to perform an unconditional kill of"
                                  " mongod, which will keep the remote server running."
                                  " Select 'mpower' to use the mFi mPower to cutoff power to"
                                  " the remote server.".format(crash_methods),
                             default="internal")


    aws_address_types = [
        "private_ip_address", "public_ip_address", "private_dns_name", "public_dns_name"]
    crash_options.add_option("--crashOption",
                             dest="crash_option",
                             help="Secondary argument for the following --crashMethod:"
                                  " 'aws_ec2': specify EC2 'address_type', which is one of {} and"
                                  " defaults to 'public_ip_address'."
                                  " 'mpower': specify output<num> to turn"
                                  " off/on, i.e., 'output1' (REQUIRED)."
                                  " 'internal': for Windows, optionally specify a crash method,"
                                  " i.e., 'notmyfault/notmyfaultc64.exe"
                                  " -accepteula crash 1'".format(aws_address_types),
                             default=None)

    crash_options.add_option("--instanceId",
                             dest="instance_id",
                             help="The instance ID of an AWS EC2 host. If specified, this instance"
                                  " will be started after a crash, if it is not in a running state."
                                  " This is required if --crashOption is 'aws_ec2'.",
                             default=None)

    crash_options.add_option("--crashWaitTime",
                             dest="crash_wait_time",
                             help="Time, in seconds, to wait before issuing crash [default:"
                                  " %default]",
                             type="int",
                             default=30)

    crash_options.add_option("--jitterForCrashWaitTime",
                             dest="crash_wait_time_jitter",
                             help="The maximum time, in seconds, to be added to --crashWaitTime,"
                                  " as a uniform distributed random value, [default: %default]",
                             type="int",
                             default=10)

    crash_options.add_option("--sshCrashUserHost",
                             dest="ssh_crash_user_host",
                             help="The crash host's user@host for performing the crash.",
                             default=None)

    crash_options.add_option("--sshCrashOption",
                             dest="ssh_crash_option",
                             help="The crash host's ssh connection options, i.e., '-i ident.pem'",
                             default=None)

    # MongoDB options
    mongodb_options.add_option("--downloadUrl",
                               dest="tarball_url",
                               help="URL of tarball to test, if unspecifed latest tarball will be"
                                    " used",
                               default="latest")

    mongodb_options.add_option("--rootDir",
                               dest="root_dir",
                               help="Root directory, on remote host, to install tarball and data"
                                    " directory [default: 'mongodb-powertest-<epochSecs>']",
                               default=None)

    mongodb_options.add_option("--mongodbBinDir",
                               dest="mongodb_bin_dir",
                               help="Directory, on remote host, containing mongoDB binaries,"
                                    " overrides bin from tarball in --downloadUrl",
                               default=None)

    mongodb_options.add_option("--dbPath",
                               dest="db_path",
                               help="Data directory to use, on remote host, if unspecified"
                                    " it will be '<rootDir>/data/db'",
                               default=None)

    mongodb_options.add_option("--logPath",
                               dest="log_path",
                               help="Log path, on remote host, if unspecified"
                                    " it will be '<rootDir>/log/mongod.log'",
                               default=None)

    # mongod options
    mongod_options.add_option("--replSet",
                              dest="repl_set",
                              help="Name of mongod single node replica set, if unpsecified mongod"
                                   " defaults to standalone node",
                              default=None)

    # The current host used to start and connect to mongod. Not meant to be specified
    # by the user.
    mongod_options.add_option("--mongodHost", dest="host", help=optparse.SUPPRESS_HELP,
                              default=None)

    # The current port used to start and connect to mongod. Not meant to be specified
    # by the user.
    mongod_options.add_option("--mongodPort",
                              dest="port",
                              help=optparse.SUPPRESS_HELP,
                              type="int",
                              default=None)

    # The ports used on the 'server' side when in standard or secret mode.
    mongod_options.add_option("--mongodUsablePorts",
                              dest="usable_ports",
                              nargs=2,
                              help="List of usable ports to be used by mongod for"
                                   " standard and secret modes, [default: %default]",
                              type="int",
                              default=[27017, 37017])

    mongod_options.add_option("--mongodOptions",
                              dest="mongod_options",
                              help="Additional mongod options",
                              default="")

    mongod_options.add_option("--fcv",
                              dest="fcv_version",
                              help="Set the FeatureCompatibilityVersion of mongod.",
                              default=None)

    mongod_options.add_option("--removeLockFile",
                              dest="remove_lock_file",
                              help="If specified, the mongod.lock file will be deleted after a"
                                   " powercycle event, before mongod is started. This is a"
                                   " workaround for mongod failing start with MMAPV1 (See"
                                   " SERVER-15109).",
                              action="store_true",
                              default=False)

    # Client options
    mongo_path = distutils.spawn.find_executable(
        "mongo", os.getcwd() + os.pathsep + os.environ["PATH"])
    client_options.add_option("--mongoPath",
                              dest="mongo_path",
                              help="Path to mongo (shell) executable, if unspecifed, mongo client"
                                   " is launched from the current directory.",
                              default=mongo_path)

    client_options.add_option("--mongoRepoRootDir",
                              dest="mongo_repo_root_dir",
                              help="Root directory of mongoDB repository, defaults to current"
                                   " directory.",
                              default=None)

    client_options.add_option("--crudClient",
                              dest="crud_client",
                              help="The path to the CRUD client script on the local host"
                                   " [default: '%default'].",
                              default="jstests/hooks/crud_client.js")

    with_external_server = "buildscripts/resmokeconfig/suites/with_external_server.yml"
    client_options.add_option("--configCrudClient",
                              dest="config_crud_client",
                              help="The path to the CRUD client configuration YML file on the"
                                   " local host. This is the resmoke.py suite file. If unspecified,"
                                   " a default configuration YML file (%default) will be used that"
                                   " provides a mongo (shell) DB connection to a running mongod.",
                              default=with_external_server)

    client_options.add_option("--numCrudClients",
                              dest="num_crud_clients",
                              help="The number of concurrent CRUD clients to run"
                                   " [default: '%default'].",
                              type="int",
                              default=1)

    client_options.add_option("--numFsmClients",
                              dest="num_fsm_clients",
                              help="The number of concurrent FSM clients to run"
                                   " [default: '%default'].",
                              type="int",
                              default=0)

    client_options.add_option("--fsmWorkloadFiles",
                              dest="fsm_workload_files",
                              help="A list of the FSM workload files to execute. More than one"
                                   " file can be specified either in a comma-delimited string,"
                                   " or by specifying this option more than once. If unspecified,"
                                   " then all FSM workload files are executed.",
                              action="append",
                              default=[])

    client_options.add_option("--fsmWorkloadBlacklistFiles",
                              dest="fsm_workload_blacklist_files",
                              help="A list of the FSM workload files to blacklist. More than one"
                                   " file can be specified either in a comma-delimited string,"
                                   " or by specifying this option more than once. Note the"
                                   " file name is the basename, i.e., 'distinct.js'.",
                              action="append",
                              default=[])

    # Program options
    program_options.add_option("--configFile",
                               dest="config_file",
                               help="YAML configuration file of program options."
                                    " Option values are mapped to command line option names."
                                    " The command line option overrides any specified options"
                                    " from this file.",
                               default=None)

    program_options.add_option("--saveConfigOptions",
                               dest="save_config_options",
                               help="Save the program options to a YAML configuration file."
                                    " If this options is specified the program only saves"
                                    " the configuration file and exits.",
                               default=None)

    program_options.add_option("--reportJsonFile", dest="report_json_file",
                               help="Create or update the specified report file upon program"
                                    " exit.",
                               default=None)

    program_options.add_option("--exitYamlFile", dest="exit_yml_file",
                               help="If specified, create a YAML file on exit containing"
                               " exit code.", default=None)

    program_options.add_option("--remotePython", dest="remote_python",
                               help="The python intepreter to use on the remote host"
                                    " [default: '%default']."
                                    " To be able to use a python virtual environment,"
                                    " which has already been provisioned on the remote"
                                    " host, specify something similar to this:"
                                    " 'source venv/bin/activate;  python'",
                               default="python")

    program_options.add_option("--remoteSudo",
                               dest="remote_sudo",
                               help="Use sudo on the remote host for priveleged operations."
                                    " [default: %default]."
                                    " For non-Windows systems, in order to perform privileged"
                                    " operations on the remote host, specify this, if the"
                                    " remote user is not able to perform root operations.",
                               action="store_true",
                               default=False)

    log_levels = ["debug", "info", "warning", "error"]
    program_options.add_option("--logLevel",
                               dest="log_level",
                               choices=log_levels,
                               help="The log level. Accepted values are: {}."
                                    " [default: '%default'].".format(log_levels),
                               default="info")

    program_options.add_option("--logFile",
                               dest="log_file",
                               help="The destination file for the log output. Defaults to stdout.",
                               default=None)

    program_options.add_option("--version",
                               dest="version",
                               help="Display this program's version",
                               action="store_true",
                               default=False)

    # Remote options, include commands and options sent from client to server under test.
    # These are 'internal' options, not meant to be directly specifed.
    # More than one remote operation can be provided and they are specified in the program args.
    program_options.add_option("--remoteOperation", dest="remote_operation",
                               help=optparse.SUPPRESS_HELP, action="store_true", default=False)

    program_options.add_option("--rsyncDest", dest="rsync_dest", nargs=2,
                               help=optparse.SUPPRESS_HELP, default=None)

    parser.add_option_group(test_options)
    parser.add_option_group(crash_options)
    parser.add_option_group(client_options)
    parser.add_option_group(mongodb_options)
    parser.add_option_group(mongod_options)
    parser.add_option_group(program_options)

    options, args = parser.parse_args()

    logging.basicConfig(format="%(asctime)s %(levelname)s %(message)s",
                        level=logging.ERROR,
                        filename=options.log_file)
    logging.getLogger(__name__).setLevel(options.log_level.upper())
    logging.Formatter.converter = time.gmtime

    LOGGER.info("powertest.py invocation: %s", " ".join(sys.argv))

    # Command line options override the config file options.
    config_options = None
    if options.config_file:
        with open(options.config_file) as ystream:
            config_options = yaml.safe_load(ystream)
        LOGGER.info("Loading config file %s with options %s", options.config_file, config_options)
        # Load the options specified in the config_file
        parser.set_defaults(**config_options)
        options, args = parser.parse_args()
        # Disable this option such that the remote side does not load a config_file.
        options.config_file = None
        config_options["config_file"] = None

    if options.save_config_options:
        # Disable this option such that the remote side does not save the config options.
        save_config_options = options.save_config_options
        options.save_config_options = None
        save_options = {}
        for opt_group in parser.option_groups:
            for opt in opt_group.option_list:
                if getattr(options, opt.dest, None) != opt.default:
                    save_options[opt.dest] = getattr(options, opt.dest, None)
        LOGGER.info("Config options being saved %s", save_options)
        with open(save_config_options, "w") as ystream:
            yaml.safe_dump(save_options, ystream, default_flow_style=False)
        sys.exit(0)

    script_name = os.path.basename(__file__)
    # Print script name and version.
    if options.version:
        print("{}:{}".format(script_name, __version__))
        sys.exit(0)

    if options.exit_yml_file:
        EXIT_YML_FILE = options.exit_yml_file
        # Disable this option such that the remote side does not generate exit_yml_file
        options.exit_yml_file = None

    if options.report_json_file:
        REPORT_JSON_FILE = options.report_json_file
        if REPORT_JSON_FILE and os.path.exists(REPORT_JSON_FILE):
            with open(REPORT_JSON_FILE) as jstream:
                REPORT_JSON = json.load(jstream)
        else:
            REPORT_JSON = {
                "failures": 0,
                "results": [
                    {"status": "fail",
                     "test_file": __name__,
                     "exit_code": 0,
                     "elapsed": 0,
                     "start": int(time.time()),
                     "end": int(time.time())}
                ]
            }
        LOGGER.debug("Updating/creating report JSON %s", REPORT_JSON)
        # Disable this option such that the remote side does not generate report.json
        options.report_json_file = None

    # Setup the crash options
    if options.crash_method == "mpower" and options.crash_option is None:
        parser.error("Missing required argument --crashOption for crashMethod '{}'".format(
            options.crash_method))

    if options.crash_method == "aws_ec2":
        if not options.instance_id:
            parser.error("Missing required argument --instanceId for crashMethod '{}'".format(
                options.crash_method))
        address_type = "public_ip_address"
        if options.crash_option:
            address_type = options.crash_option
        if address_type not in aws_address_types:
            parser.error("Invalid crashOption address_type '{}' specified for crashMethod"
                         " 'aws_ec2', specify one of {}".format(address_type, aws_address_types))

    # Initialize the mongod options
    # Note - We use posixpath for Windows client to Linux server scenarios.
    if not options.root_dir:
        options.root_dir = "mongodb-powertest-{}".format(int(time.time()))
    if not options.db_path:
        options.db_path = posixpath.join(options.root_dir, "data", "db")
    if not options.log_path:
        options.log_path = posixpath.join(options.root_dir, "log", "mongod.log")
    mongod_options_map = parse_options(options.mongod_options)
    set_fcv_cmd = "set_fcv" if options.fcv_version is not None else ""
    remove_lock_file_cmd = "remove_lock_file" if options.remove_lock_file else ""

    # Error out earlier if these options are not properly specified
    write_concern = yaml.safe_load(options.write_concern)
    options.canary_doc = yaml.safe_load(options.canary_doc)

    # Invoke remote_handler if remote_operation is specified.
    # The remote commands are program args.
    if options.remote_operation:
        ret = remote_handler(options, args)
        # Exit here since the local operations are performed after this.
        local_exit(ret)

    # Required option for non-remote commands.
    if options.ssh_user_host is None and not options.remote_operation:
        parser.error("Missing required argument --sshUserHost")

    secret_port = options.usable_ports[1]
    standard_port = options.usable_ports[0]

    seed_docs = "seed_docs" if options.seed_doc_num else ""

    if options.rsync_data:
        rsync_cmd = "rsync_data"
        backup_path_before = options.backup_path_before
        if not backup_path_before:
            backup_path_before = "{}/data-beforerecovery".format(options.root_dir)
        backup_path_after = options.backup_path_after
        if not backup_path_after:
            backup_path_after = "{}/data-afterrecovery".format(options.root_dir)
        # Set the first backup directory, for loop 1.
        backup_path_before = "{}-1".format(backup_path_before)
        backup_path_after = "{}-1".format(backup_path_after)
    else:
        rsync_cmd = ""
        rsync_opt = ""

    # Setup the mongo client, mongo_path is required if there are local clients.
    if (options.num_crud_clients > 0 or
            options.num_fsm_clients > 0 or
            options.validate_collections == "local"):
        if not options.mongo_path:
            LOGGER.error("mongoPath must be specified")
            local_exit(1)
        if not os.path.isfile(options.mongo_path):
            LOGGER.error("mongoPath %s does not exist", options.mongo_path)
            local_exit(1)
        mongo_path = os.path.abspath(os.path.normpath(options.mongo_path))

    # Setup the CRUD & FSM clients.
    if not os.path.isfile(options.config_crud_client):
        LOGGER.error("configCrudClient %s does not exist", options.config_crud_client)
        local_exit(1)
    with_external_server = "buildscripts/resmokeconfig/suites/with_external_server.yml"
    fsm_client = "jstests/libs/fsm_serial_client.js"
    fsm_workload_files = []
    for fsm_workload_file in options.fsm_workload_files:
        fsm_workload_files += fsm_workload_file.replace(" ", "").split(",")
    fsm_workload_blacklist_files = []
    for fsm_workload_blacklist_file in options.fsm_workload_blacklist_files:
        fsm_workload_blacklist_files += fsm_workload_blacklist_file.replace(" ", "").split(",")
    read_concern_level = options.read_concern_level
    if write_concern and not read_concern_level:
        read_concern_level = "local"
    crud_test_data = {}
    if read_concern_level:
        crud_test_data["defaultReadConcernLevel"] = read_concern_level
    if write_concern:
        crud_test_data["defaultWriteConcern"] = write_concern
    if read_concern_level or write_concern:
        eval_str = "load('jstests/libs/override_methods/set_read_and_write_concerns.js');"
    else:
        eval_str = ""
    fsm_test_data = copy.deepcopy(crud_test_data)
    fsm_test_data["fsmDbBlacklist"] = [options.db_name]
    if fsm_workload_files:
        fsm_test_data["workloadFiles"] = fsm_workload_files
    if fsm_workload_blacklist_files:
        fsm_test_data["workloadBlacklistFiles"] = fsm_workload_blacklist_files
    crud_test_data["dbName"] = options.db_name

    # Setup the mongo_repo_root.
    if options.mongo_repo_root_dir:
        mongo_repo_root_dir = options.mongo_repo_root_dir
    else:
        mongo_repo_root_dir = os.getcwd()
    if not os.path.isdir(mongo_repo_root_dir):
        LOGGER.error("mongoRepoRoot %s does not exist", mongo_repo_root_dir)
        local_exit(1)

    # Setup the validate_collections option.
    if options.validate_collections == "remote":
        validate_collections_cmd = "validate_collections"
    else:
        validate_collections_cmd = ""

    # Setup the validate_canary option.
    if options.canary and "nojournal" in mongod_options_map:
        LOGGER.error("Cannot create and validate canary documents if the mongod option"
                     " '--nojournal' is used.")
        local_exit(1)

    internal_crash_options = ["internal", "kill"]
    if options.canary == "remote" and options.crash_method != "internal":
        parser.error("The option --canary can only be specified as 'remote' if --crashMethod"
                     " is one of {}".format(internal_crash_options))
    orig_canary_doc = canary_doc = ""
    validate_canary_cmd = ""

    # Set the Pymongo connection timeout to 1 hour for canary insert & validation.
    one_hour_ms = 60 * 60 * 1000

    # The remote mongod host comes from the ssh_user_host,
    # which may be specified as user@host.
    ssh_user_host = options.ssh_user_host
    ssh_user, ssh_host = get_user_host(ssh_user_host)
    mongod_host = ssh_host

    ssh_connection_options = "{} {}".format(
        default_ssh_connection_options,
        options.ssh_connection_options if options.ssh_connection_options else "")
    # For remote operations requiring sudo, force pseudo-tty allocation,
    # see https://stackoverflow.com/questions/10310299/proper-way-to-sudo-over-ssh.
    # Note - the ssh option RequestTTY was added in OpenSSH 5.9, so we use '-tt'.
    ssh_options = "-tt" if options.remote_sudo else None

    # Establish EC2 connection if an instance_id is specified.
    if options.instance_id:
        ec2 = aws_ec2.AwsEc2()
        # Determine address_type if not using 'aws_ec2' crash_method.
        if options.crash_method != "aws_ec2":
            address_type = "public_ip_address"
            ret, aws_status = ec2.control_instance(mode="status", image_id=options.instance_id)
            if not is_instance_running(ret, aws_status):
                LOGGER.error("AWS instance is not running:  %d %s", ret, aws_status)
                local_exit(1)
            if (ssh_host == aws_status.private_ip_address
                    or ssh_host == aws_status.private_dns_name):
                address_type = "private_ip_address"

    # Instantiate the local handler object.
    local_ops = LocalToRemoteOperations(user_host=ssh_user_host,
                                        ssh_connection_options=ssh_connection_options,
                                        ssh_options=ssh_options, use_shell=True)
    verify_remote_access(local_ops)

    # Bootstrap the remote host with this script.
    ret, output = local_ops.copy_to(__file__)
    if ret:
        LOGGER.error("Cannot access remote system %s", output)
        local_exit(1)

    # Pass client_args to the remote script invocation.
    client_args = ""
    for option in parser._get_all_options():
        if option.dest:
            option_value = getattr(options, option.dest, None)
            if option_value != option.default:
                # The boolean options do not require the option_value.
                if isinstance(option_value, bool):
                    option_value = ""
                # Quote the non-default option values from the invocation of this script,
                # if they have spaces, or quotes, such that they can be safely passed to the
                # remote host's invocation of this script.
                elif isinstance(option_value, str) and re.search("\"|'| ", option_value):
                    option_value = "'{}'".format(option_value)
                # The tuple, list or set options need to be changed to a string.
                elif isinstance(option_value, (tuple, list, set)):
                    option_value = " ".join(map(str, option_value))
                client_args = "{} {} {}".format(client_args, option.get_opt_string(), option_value)

    LOGGER.info("%s %s", __file__, client_args)

    # Remote install of MongoDB.
    ret, output = call_remote_operation(
        local_ops,
        options.remote_python,
        script_name,
        client_args,
        "--remoteOperation install_mongod")
    LOGGER.info("****install_mongod: %d %s****", ret, output)
    if ret:
        local_exit(ret)

    # test_time option overrides num_loops.
    if options.test_time:
        options.num_loops = 999999
    else:
        options.test_time = 999999
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
        if options.canary and loop_num > 1:
            if options.canary == "remote":
                canary_opt = "--docForCanary \"{}\"".format(canary_doc)
                validate_canary_cmd = "validate_canary" if options.canary else ""
            else:
                validate_canary_local = True
        else:
            canary_opt = ""

        # Since rsync requires Posix style paths, we do not use os.path.join to
        # construct the rsync destination directory.
        if rsync_cmd:
            new_path_dir = get_backup_path(backup_path_before, loop_num)
            rsync_opt = "--rsyncDest {} {}".format(backup_path_before, new_path_dir)
            backup_path_before = new_path_dir

        # Optionally, rsync the pre-recovery database.
        # Start monogd on the secret port.
        # Optionally validate collections, validate the canary and seed the collection.
        remote_operation = ("--remoteOperation"
                            " {rsync_opt}"
                            " {canary_opt}"
                            " --mongodHost {host}"
                            " --mongodPort {port}"
                            " {rsync_cmd}"
                            " {remove_lock_file_cmd}"
                            " start_mongod"
                            " {set_fcv_cmd}"
                            " {validate_collections_cmd}"
                            " {validate_canary_cmd}"
                            " {seed_docs}").format(
                                rsync_opt=rsync_opt, canary_opt=canary_opt, host=mongod_host,
                                port=secret_port, rsync_cmd=rsync_cmd,
                                remove_lock_file_cmd=remove_lock_file_cmd, set_fcv_cmd=set_fcv_cmd
                                if loop_num == 1 else "",
                                validate_collections_cmd=validate_collections_cmd,
                                validate_canary_cmd=validate_canary_cmd,
                                seed_docs=seed_docs if loop_num == 1 else "")
        ret, output = call_remote_operation(
            local_ops,
            options.remote_python,
            script_name,
            client_args,
            remote_operation)
        rsync_text = "rsync_data beforerecovery & " if options.rsync_data else ""
        LOGGER.info("****%sstart mongod: %d %s****", rsync_text, ret, output)
        if ret:
            local_exit(ret)

        # Optionally validate canary document locally.
        if validate_canary_local:
            mongo = pymongo.MongoClient(**get_mongo_client_args(
                host=mongod_host, port=secret_port, server_selection_timeout_ms=one_hour_ms,
                socket_timeout_ms=one_hour_ms))
            ret = mongo_validate_canary(mongo, options.db_name, options.collection_name, canary_doc)
            LOGGER.info("Local canary validation: %d", ret)
            if ret:
                local_exit(ret)

        # Optionally, run local validation of collections.
        if options.validate_collections == "local":
            host_port = "{}:{}".format(mongod_host, secret_port)
            new_config_file = NamedTempFile.create(suffix=".yml", directory="tmp")
            temp_client_files.append(new_config_file)
            validation_test_data = {"skipValidationOnNamespaceNotFound": True}
            new_resmoke_config(with_external_server, new_config_file, validation_test_data)
            ret, output = resmoke_client(
                mongo_repo_root_dir,
                mongo_path,
                host_port,
                "jstests/hooks/run_validate_collections.js",
                new_config_file)
            LOGGER.info("Local collection validation: %d %s", ret, output)
            if ret:
                local_exit(ret)

        # Shutdown mongod on secret port.
        remote_op = ("--remoteOperation"
                     " --mongodPort {}"
                     " shutdown_mongod").format(secret_port)
        ret, output = call_remote_operation(
            local_ops,
            options.remote_python,
            script_name,
            client_args,
            remote_op)
        LOGGER.info("****shutdown_mongod: %d %s****", ret, output)
        if ret:
            local_exit(ret)

        # Since rsync requires Posix style paths, we do not use os.path.join to
        # construct the rsync destination directory.
        if rsync_cmd:
            new_path_dir = get_backup_path(backup_path_after, loop_num)
            rsync_opt = "--rsyncDest {} {}".format(backup_path_after, new_path_dir)
            backup_path_after = new_path_dir

        # Optionally, rsync the post-recovery database.
        # Start monogd on the standard port.
        remote_op = ("--remoteOperation"
                     " {}"
                     " --mongodHost {}"
                     " --mongodPort {}"
                     " {}"
                     " start_mongod").format(rsync_opt, mongod_host, standard_port, rsync_cmd)
        ret, output = call_remote_operation(local_ops, options.remote_python, script_name,
                                            client_args, remote_op)
        rsync_text = "rsync_data afterrecovery & " if options.rsync_data else ""
        LOGGER.info("****%s start mongod: %d %s****", rsync_text, ret, output)
        if ret:
            local_exit(ret)

        boot_time_after_recovery = get_boot_datetime(output)

        # Start CRUD clients
        host_port = "{}:{}".format(mongod_host, standard_port)
        for i in xrange(options.num_crud_clients):
            if options.config_crud_client == with_external_server:
                crud_config_file = NamedTempFile.create(suffix=".yml", directory="tmp")
                crud_test_data["collectionName"] = "{}-{}".format(options.collection_name, i)
                new_resmoke_config(
                    with_external_server, crud_config_file, crud_test_data, eval_str)
            else:
                crud_config_file = options.config_crud_client
            _, _ = resmoke_client(
                work_dir=mongo_repo_root_dir,
                mongo_path=mongo_path,
                host_port=host_port,
                js_test=options.crud_client,
                resmoke_suite=crud_config_file,
                repeat_num=100,
                no_wait=True,
                log_file="crud_{}.log".format(i))

        if options.num_crud_clients:
            LOGGER.info("****Started %d CRUD client(s)****", options.num_crud_clients)

        # Start FSM clients
        for i in xrange(options.num_fsm_clients):
            fsm_config_file = NamedTempFile.create(suffix=".yml", directory="tmp")
            fsm_test_data["dbNamePrefix"] = "fsm-{}".format(i)
            # Do collection validation only for the first FSM client.
            fsm_test_data["validateCollections"] = True if i == 0 else False
            new_resmoke_config(with_external_server, fsm_config_file, fsm_test_data, eval_str)
            _, _ = resmoke_client(
                work_dir=mongo_repo_root_dir,
                mongo_path=mongo_path,
                host_port=host_port,
                js_test=fsm_client,
                resmoke_suite=fsm_config_file,
                repeat_num=100,
                no_wait=True,
                log_file="fsm_{}.log".format(i))

        if options.num_fsm_clients:
            LOGGER.info("****Started %d FSM client(s)****", options.num_fsm_clients)

        # Crash the server. A pre-crash canary document is optionally written to the DB.
        crash_canary = {}
        if options.canary:
            canary_doc = {"x": time.time()}
            orig_canary_doc = copy.deepcopy(canary_doc)
            mongo = pymongo.MongoClient(**get_mongo_client_args(
                host=mongod_host, port=standard_port, server_selection_timeout_ms=one_hour_ms,
                socket_timeout_ms=one_hour_ms))
            crash_canary["function"] = mongo_insert_canary
            crash_canary["args"] = [mongo, options.db_name, options.collection_name, canary_doc]
        ret, output = crash_server_or_kill_mongod(options, crash_canary, standard_port, local_ops,
                                                  script_name, client_args)

        LOGGER.info("Crash server or Kill mongod: %d %s****", ret, output)

        # For internal crashes 'ret' is non-zero, because the ssh session unexpectedly terminates.
        if options.crash_method != "internal" and ret:
            raise Exception("Crash of server failed: {}", format(output))

        if options.crash_method != "kill":
            # Check if the crash failed due to an ssh error.
            if options.crash_method == "internal" and local_ops.ssh_error(output):
                ssh_failure_exit(ret, output)
            # Wait a bit after sending command to crash the server to avoid connecting to the
            # server before the actual crash occurs.
            time.sleep(10)

        # Kill any running clients and cleanup temporary files.
        Processes.kill_all()
        for temp_file in temp_client_files:
            NamedTempFile.delete(temp_file)

        instance_running = True
        if options.instance_id:
            ret, aws_status = ec2.control_instance(mode="status", image_id=options.instance_id)
            LOGGER.info("AWS EC2 instance status: %d %s****", ret, aws_status)
            instance_running = is_instance_running(ret, aws_status)

        # The EC2 instance address changes if the instance is restarted.
        if options.crash_method == "aws_ec2" or not instance_running:
            ret, aws_status = ec2.control_instance(
                mode="start", image_id=options.instance_id, wait_time_secs=600, show_progress=True)
            LOGGER.info("Start instance: %d %s****", ret, aws_status)
            if ret:
                raise Exception("Start instance failed: {}".format(aws_status))
            if not hasattr(aws_status, address_type):
                raise Exception("Cannot determine address_type {} from AWS EC2 status {}".format(
                    address_type, aws_status))
            ssh_host = getattr(aws_status, address_type)
            if ssh_user is None:
                ssh_user_host = ssh_host
            else:
                ssh_user_host = "{}@{}".format(ssh_user, ssh_host)
            mongod_host = ssh_host

        # Reestablish remote access after crash.
        local_ops = LocalToRemoteOperations(user_host=ssh_user_host,
                                            ssh_connection_options=ssh_connection_options,
                                            ssh_options=ssh_options, use_shell=True)
        verify_remote_access(local_ops)
        ret, output = call_remote_operation(local_ops, options.remote_python, script_name,
                                            client_args, "--remoteOperation noop")
        boot_time_after_crash = get_boot_datetime(output)
        if boot_time_after_crash == -1 or boot_time_after_recovery == -1:
            LOGGER.warning(
                "Cannot compare boot time after recovery: %s with boot time after crash: %s",
                boot_time_after_recovery, boot_time_after_crash)
        elif options.crash_method != "kill" and boot_time_after_crash <= boot_time_after_recovery:
            raise Exception(
                "System boot time after crash ({}) is not newer than boot time before crash ({})".
                format(boot_time_after_crash, boot_time_after_recovery))

        canary_doc = copy.deepcopy(orig_canary_doc)

        test_time = int(time.time()) - start_time
        LOGGER.info("****Completed test loop %d test time %d seconds****", loop_num, test_time)
        if loop_num == options.num_loops or test_time >= options.test_time:
            break

    REPORT_JSON_SUCCESS = True
    local_exit(0)


if __name__ == "__main__":
    main()
