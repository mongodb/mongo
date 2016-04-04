#!/usr/bin/env python
"""
A script that provides:
1. Ability to grab binaries where possible from LLVM.
2. Ability to download binaries from MongoDB cache for clang-format.
3. Validates clang-format is the right version.
4. Has support for checking which files are to be checked.
5. Supports validating and updating a set of files to the right coding style.
"""
from __future__ import print_function, absolute_import

import Queue
import difflib
import itertools
import os
import re
import shutil
import string
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib
from distutils import spawn
from optparse import OptionParser
from multiprocessing import cpu_count

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts import moduleconfig


##############################################################################
#
# Constants for clang-format
#
#

# Expected version of clang-format
CLANG_FORMAT_VERSION = "3.6.0"

# Name of clang-format as a binary
CLANG_FORMAT_PROGNAME = "clang-format"

# URL location of the "cached" copy of clang-format to download
# for users which do not have clang-format installed
CLANG_FORMAT_HTTP_LINUX_CACHE = "https://s3.amazonaws.com/boxes.10gen.com/build/clang-format-rhel55.tar.gz"

# URL on LLVM's website to download the clang tarball
CLANG_FORMAT_SOURCE_URL_BASE = string.Template("http://llvm.org/releases/$version/clang+llvm-$version-$llvm_distro.tar.xz")

# Path in the tarball to the clang-format binary
CLANG_FORMAT_SOURCE_TAR_BASE = string.Template("clang+llvm-$version-$tar_path/bin/" + CLANG_FORMAT_PROGNAME)

# Path to the modules in the mongodb source tree
# Has to match the string in SConstruct
MODULE_DIR = "src/mongo/db/modules"

# Copied from python 2.7 version of subprocess.py
# Exception classes used by this module.
class CalledProcessError(Exception):
    """This exception is raised when a process run by check_call() or
    check_output() returns a non-zero exit status.
    The exit status will be stored in the returncode attribute;
    check_output() will also store the output in the output attribute.
    """
    def __init__(self, returncode, cmd, output=None):
        self.returncode = returncode
        self.cmd = cmd
        self.output = output
    def __str__(self):
        return ("Command '%s' returned non-zero exit status %d with output %s" %
            (self.cmd, self.returncode, self.output))


# Copied from python 2.7 version of subprocess.py
def check_output(*popenargs, **kwargs):
    r"""Run command with arguments and return its output as a byte string.

    If the exit code was non-zero it raises a CalledProcessError.  The
    CalledProcessError object will have the return code in the returncode
    attribute and output in the output attribute.

    The arguments are the same as for the Popen constructor.  Example:

    >>> check_output(["ls", "-l", "/dev/null"])
    'crw-rw-rw- 1 root root 1, 3 Oct 18  2007 /dev/null\n'

    The stdout argument is not allowed as it is used internally.
    To capture standard error in the result, use stderr=STDOUT.

    >>> check_output(["/bin/sh", "-c",
    ...               "ls -l non_existent_file ; exit 0"],
    ...              stderr=STDOUT)
    'ls: non_existent_file: No such file or directory\n'
    """
    if 'stdout' in kwargs:
        raise ValueError('stdout argument not allowed, it will be overridden.')
    process = subprocess.Popen(stdout=subprocess.PIPE, *popenargs, **kwargs)
    output, unused_err = process.communicate()
    retcode = process.poll()
    if retcode:
        cmd = kwargs.get("args")
        if cmd is None:
            cmd = popenargs[0]
        raise CalledProcessError(retcode, cmd, output)
    return output

def callo(args):
    """Call a program, and capture its output
    """
    return check_output(args)

def get_llvm_url(version, llvm_distro):
    """Get the url to download clang-format from llvm.org
    """
    return CLANG_FORMAT_SOURCE_URL_BASE.substitute(
        version=version,
        llvm_distro=llvm_distro)

def get_tar_path(version, tar_path):
    """ Get the path to clang-format in the llvm tarball
    """
    return CLANG_FORMAT_SOURCE_TAR_BASE.substitute(
        version=version,
        tar_path=tar_path)

def extract_clang_format(tar_path):
    # Extract just the clang-format binary
    # On OSX, we shell out to tar because tarfile doesn't support xz compression
    if sys.platform == 'darwin':
         subprocess.call(['tar', '-xzf', tar_path, '*clang-format*'])
    # Otherwise we use tarfile because some versions of tar don't support wildcards without
    # a special flag
    else:
        tarfp = tarfile.open(tar_path)
        for name in tarfp.getnames():
            if name.endswith('clang-format'):
                tarfp.extract(name)
        tarfp.close()

def get_clang_format_from_llvm(llvm_distro, tar_path, dest_file):
    """Download clang-format from llvm.org, unpack the tarball,
    and put clang-format in the specified place
    """
    # Build URL
    url = get_llvm_url(CLANG_FORMAT_VERSION, llvm_distro)

    dest_dir = tempfile.gettempdir()
    temp_tar_file = os.path.join(dest_dir, "temp.tar.xz")

    # Download from LLVM
    print("Downloading clang-format %s from %s, saving to %s" % (CLANG_FORMAT_VERSION,
            url, temp_tar_file))
    urllib.urlretrieve(url, temp_tar_file)

    extract_clang_format(temp_tar_file)

    # Destination Path
    shutil.move(get_tar_path(CLANG_FORMAT_VERSION, tar_path), dest_file)

def get_clang_format_from_linux_cache(dest_file):
    """Get clang-format from mongodb's cache
    """
    # Get URL
    url = CLANG_FORMAT_HTTP_LINUX_CACHE

    dest_dir = tempfile.gettempdir()
    temp_tar_file = os.path.join(dest_dir, "temp.tar.xz")

    # Download the file
    print("Downloading clang-format %s from %s, saving to %s" % (CLANG_FORMAT_VERSION,
            url, temp_tar_file))
    urllib.urlretrieve(url, temp_tar_file)

    extract_clang_format(temp_tar_file)

    # Destination Path
    shutil.move("llvm/Release/bin/clang-format", dest_file)

class ClangFormat(object):
    """Class encapsulates finding a suitable copy of clang-format,
    and linting/formating an individual file
    """
    def __init__(self, path, cache_dir):
        if os.path.exists('/usr/bin/clang-format-3.6'):
            clang_format_progname = 'clang-format-3.6'
        else:
            clang_format_progname = CLANG_FORMAT_PROGNAME

        # Initialize clang-format configuration information
        if sys.platform.startswith("linux"):
              #"3.6.0/clang+llvm-3.6.0-x86_64-linux-gnu-ubuntu-14.04.tar.xz
            self.platform = "linux_x64"
            self.llvm_distro = "x86_64-linux-gnu-ubuntu"
            self.tar_path = "x86_64-linux-gnu"
        elif sys.platform == "win32":
            self.platform = "windows_x64"
            self.llvm_distro = "windows_x64"
            self.tar_path = None
            clang_format_progname += ".exe"
        elif sys.platform == "darwin":
             #"3.6.0/clang+llvm-3.6.0-x86_64-apple-darwin.tar.xz
            self.platform = "darwin_x64"
            self.llvm_distro = "x86_64-apple-darwin"
            self.tar_path = "x86_64-apple-darwin"

        self.path = None

        # Find Clang-Format now
        if path is not None:
            if os.path.isfile(path):
                self.path = path
            else:
                print("WARNING: Could not find clang-format %s" % (path))

        # Check the envionrment variable
        if "MONGO_CLANG_FORMAT" in os.environ:
            self.path = os.environ["MONGO_CLANG_FORMAT"]

            if self.path and not self._validate_version(warn=True):
                self.path = None

        # Check the users' PATH environment variable now
        if self.path is None:
            self.path = spawn.find_executable(clang_format_progname)

            if self.path and not self._validate_version(warn=True):
                self.path = None

        # If Windows, try to grab it from Program Files
        if sys.platform == "win32":
            win32bin = os.path.join(os.environ["ProgramFiles(x86)"], "LLVM\\bin\\clang-format.exe")
            if os.path.exists(win32bin):
                self.path = win32bin

        # Have not found it yet, download it from the web
        if self.path is None:
            if not os.path.isdir(cache_dir):
                os.makedirs(cache_dir)

            self.path = os.path.join(cache_dir, clang_format_progname)

            if not os.path.isfile(self.path):
                if sys.platform.startswith("linux"):
                    get_clang_format_from_linux_cache(self.path)
                elif sys.platform == "darwin":
                    get_clang_format_from_llvm(self.llvm_distro, self.tar_path, self.path)
                else:
                    print("ERROR: clang-format.py does not support downloading clang-format " +
                        " on this platform, please install clang-format " + CLANG_FORMAT_VERSION)

        # Validate we have the correct version
        self._validate_version()

        self.print_lock = threading.Lock()

    def _validate_version(self, warn=False):
        """Validate clang-format is the expected version
        """
        cf_version = callo([self.path, "--version"])

        if CLANG_FORMAT_VERSION in cf_version:
            return True

        if warn:
            print("WARNING: clang-format found in path, but incorrect version found at " +
                    self.path + " with version: " + cf_version)

        return False

    def _lint(self, file_name, print_diff):
        """Check the specified file has the correct format
        """
        with open(file_name, 'rb') as original_text:
            original_file = original_text.read()

        # Get formatted file as clang-format would format the file
        formatted_file = callo([self.path, "--style=file", file_name])

        if original_file != formatted_file:
            if print_diff:
                original_lines = original_file.splitlines()
                formatted_lines = formatted_file.splitlines()
                result = difflib.unified_diff(original_lines, formatted_lines)

                # Take a lock to ensure diffs do not get mixed when printed to the screen
                with self.print_lock:
                    print("ERROR: Found diff for " + file_name)
                    print("To fix formatting errors, run %s --style=file -i %s" %
                            (self.path, file_name))
                    for line in result:
                        print(line.rstrip())

            return False

        return True

    def lint(self, file_name):
        """Check the specified file has the correct format
        """
        return self._lint(file_name, print_diff=True)

    def format(self, file_name):
        """Update the format of the specified file
        """
        if self._lint(file_name, print_diff=False):
            return True

        # Update the file with clang-format
        return not subprocess.call([self.path, "--style=file", "-i", file_name])


def parallel_process(items, func):
    """Run a set of work items to completion
    """
    try:
        cpus = cpu_count()
    except NotImplementedError:
        cpus = 1

    task_queue = Queue.Queue()

    # Use a list so that worker function will capture this variable
    pp_event = threading.Event()
    pp_result = [True]
    pp_lock = threading.Lock()

    def worker():
        """Worker thread to process work items in parallel
        """
        while not pp_event.is_set():
            try:
                item = task_queue.get_nowait()
            except Queue.Empty:
                # if the queue is empty, exit the worker thread
                pp_event.set()
                return

            try:
                ret = func(item)
            finally:
                # Tell the queue we finished with the item
                task_queue.task_done()

            # Return early if we fail, and signal we are done
            if not ret:
                with pp_lock:
                    pp_result[0] = False

                pp_event.set()
                return

    # Enqueue all the work we want to process
    for item in items:
        task_queue.put(item)

    # Process all the work
    threads = []
    for cpu in range(cpus):
        thread = threading.Thread(target=worker)

        thread.daemon = True
        thread.start()
        threads.append(thread)

    # Wait for the threads to finish
    # Loop with a timeout so that we can process Ctrl-C interrupts
    # Note: On Python 2.6 wait always returns None so we check is_set also,
    #  This works because we only set the event once, and never reset it
    while not pp_event.wait(1) and not pp_event.is_set():
        time.sleep(1)

    for thread in threads:
        thread.join()

    return pp_result[0]

def get_base_dir():
    """Get the base directory for mongo repo.
        This script assumes that it is running in buildscripts/, and uses
        that to find the base directory.
    """
    try:
        return subprocess.check_output(['git', 'rev-parse', '--show-toplevel']).rstrip()
    except:
        # We are not in a valid git directory. Use the script path instead.
        return os.path.dirname(os.path.dirname(os.path.realpath(__file__)))

def get_repos():
    """Get a list of Repos to check clang-format for
    """
    base_dir = get_base_dir()

    # Get a list of modules
    # TODO: how do we filter rocks, does it matter?
    mongo_modules = moduleconfig.discover_module_directories(
                        os.path.join(base_dir, MODULE_DIR), None)

    paths = [os.path.join(base_dir, MODULE_DIR, m) for m in mongo_modules]

    paths.append(base_dir)

    return [Repo(p) for p in paths]


class Repo(object):
    """Class encapsulates all knowledge about a git repository, and its metadata
        to run clang-format.
    """
    def __init__(self, path):
        self.path = path

        self.root = self._get_root()

    def _callgito(self, args):
        """Call git for this repository
        """
        # These two flags are the equivalent of -C in newer versions of Git
        # but we use these to support versions pre 1.8.5 but it depends on the command
        # and what the current directory is
        return callo(['git', '--git-dir', os.path.join(self.path, ".git"),
                            '--work-tree', self.path] + args)

    def get_candidates(self):
        """Get the set of candidate files to check by querying the repository

        Returns the full path to the file for clang-format to consume.
        """
        # NOTE: Files may have an absolute root (i.e. leading /)
        valid_files = list(self.get_candidate_files())

        # Get the full file name here
        valid_files = [os.path.normpath(os.path.join(self.root, f)) for f in valid_files]

        return valid_files

    def get_root(self):
        """Get the root directory for this repository
        """
        return self.root

    def _get_root(self):
        """Gets the root directory for this repository from git
        """
        gito = self._callgito(['rev-parse', '--show-toplevel'])

        return gito.rstrip()

    def _git_ls_files(self, cmd):
        """Run git-ls-files and filter the list of files to a valid candidate list
        """
        gito = self._callgito(cmd)

        # This allows us to pick all the interesting files
        # in the mongo and mongo-enterprise repos
        file_list = [line.rstrip()
                for line in gito.splitlines()
                    if (line.startswith("jstests") or line.startswith("src"))
                        and not line.startswith("src/third_party")]

        files_match = re.compile('\\.(h|cpp|js)$')

        file_list = [a for a in file_list if files_match.search(a)]

        return file_list

    def get_candidate_files(self):
        """Query git to get a list of all files in the repo to consider for analysis
        """
        return self._git_ls_files(["ls-files", "--cached"])

    def get_working_tree_candidate_files(self):
        """Query git to get a list of all files in the working tree to consider for analysis
        """
        return self._git_ls_files(["ls-files", "--cached", "--others"])

    def get_working_tree_candidates(self):
        """Get the set of candidate files to check by querying the repository

        Returns the full path to the file for clang-format to consume.
        """
        valid_files = list(self.get_working_tree_candidate_files())

        # Get the full file name here
        valid_files = [os.path.normpath(os.path.join(self.root, f)) for f in valid_files]

        return valid_files

def get_files_to_check_working_tree():
    """Get a list of files to check form the working tree.
       This will pick up files not managed by git.
    """
    repos = get_repos()

    valid_files = list(itertools.chain.from_iterable([r.get_working_tree_candidates() for r in repos]))

    return valid_files

def get_files_to_check():
    """Get a list of files that need to be checked
       based on which files are managed by git.
    """
    repos = get_repos()

    valid_files = list(itertools.chain.from_iterable([r.get_candidates() for r in repos]))

    return valid_files

def get_files_to_check_from_patch(patches):
    """Take a patch file generated by git diff, and scan the patch for a list of files to check.
    """
    candidates = []

    # Get a list of candidate_files
    check = re.compile(r"^diff --git a\/([a-z\/\.\-_0-9]+) b\/[a-z\/\.\-_0-9]+")

    lines = []
    for patch in patches:
        with open(patch, "rb") as infile:
            lines += infile.readlines()

    candidates = [check.match(line).group(1) for line in lines if check.match(line)]

    repos = get_repos()

    valid_files = list(itertools.chain.from_iterable([r.get_candidates(candidates) for r in repos]))

    return valid_files

def _get_build_dir():
    """Get the location of the scons' build directory in case we need to download clang-format
    """
    return os.path.join(get_base_dir(), "build")

def _lint_files(clang_format, files):
    """Lint a list of files with clang-format
    """
    clang_format = ClangFormat(clang_format, _get_build_dir())

    lint_clean = parallel_process([os.path.abspath(f) for f in files], clang_format.lint)

    if not lint_clean:
        print("ERROR: Code Style does not match coding style")
        sys.exit(1)

def lint_patch(clang_format, infile):
    """Lint patch command entry point
    """
    files = get_files_to_check_from_patch(infile)

    # Patch may have files that we do not want to check which is fine
    if files:
        _lint_files(clang_format, files)

def lint(clang_format):
    """Lint files command entry point
    """
    files = get_files_to_check()

    _lint_files(clang_format, files)

    return True

def lint_all(clang_format):
    """Lint files command entry point based on working tree
    """
    files = get_files_to_check_working_tree()

    _lint_files(clang_format, files)

    return True

def _format_files(clang_format, files):
    """Format a list of files with clang-format
    """
    clang_format = ClangFormat(clang_format, _get_build_dir())

    format_clean = parallel_process([os.path.abspath(f) for f in files], clang_format.format)

    if not format_clean:
        print("ERROR: failed to format files")
        sys.exit(1)

def format_func(clang_format):
    """Format files command entry point
    """
    files = get_files_to_check()

    _format_files(clang_format, files)

def usage():
    """Print usage
    """
    print("clang-format.py supports 4 commands [ lint, lint-all, lint-patch, format ].")

def main():
    """Main entry point
    """
    parser = OptionParser()
    parser.add_option("-c", "--clang-format", type="string", dest="clang_format")

    (options, args) = parser.parse_args(args=sys.argv)

    if len(args) > 1:
        command = args[1]

        if command == "lint":
            lint(options.clang_format)
        elif command == "lint-all":
            lint_all(options.clang_format)
        elif command == "lint-patch":
            lint_patch(options.clang_format, args[2:])
        elif command == "format":
            format_func(options.clang_format)
        else:
            usage()
    else:
        usage()

if __name__ == "__main__":
    main()
