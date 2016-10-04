#!/usr/bin/env python
"""
eslint.py
 Will download a prebuilt ESLint binary if necessary (i.e. it isn't installed, isn't in the current
 path, or is the wrong version). It works in much the same way as clang_format.py. In lint mode, it
 will lint the files or directory paths passed. In lint-patch mode, for upload.py, it will see if
 there are any candidate files in the supplied patch. Fix mode will run ESLint with the --fix
 option, and that will update the files with missing semicolons and similar repairable issues.
 There is also a -d mode that assumes you only want to run one copy of ESLint per file / directory
 parameter supplied. This lets ESLint search for candidate files to lint.
"""
import Queue
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
from multiprocessing import cpu_count
from optparse import OptionParser

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts.resmokelib.utils import globstar
from buildscripts import moduleconfig


##############################################################################
#
# Constants for ESLint
#
#

# Expected version of ESLint.
ESLINT_VERSION = "2.3.0"

# Name of ESLint as a binary.
ESLINT_PROGNAME = "eslint"

# URL location of our provided ESLint binaries.
ESLINT_HTTP_LINUX_CACHE = "https://s3.amazonaws.com/boxes.10gen.com/build/eslint-" + \
                           ESLINT_VERSION + "-linux.tar.gz"
ESLINT_HTTP_DARWIN_CACHE = "https://s3.amazonaws.com/boxes.10gen.com/build/eslint-" + \
                            ESLINT_VERSION + "-darwin.tar.gz"

# Path in the tarball to the ESLint binary.
ESLINT_SOURCE_TAR_BASE = string.Template(ESLINT_PROGNAME + "-$platform-$arch")

# Path to the modules in the mongodb source tree.
# Has to match the string in SConstruct.
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

def extract_eslint(tar_path, target_file):
    tarfp = tarfile.open(tar_path)
    for name in tarfp.getnames():
        if name == target_file:
            tarfp.extract(name)
    tarfp.close()

def get_eslint_from_cache(dest_file, platform, arch):
    """Get ESLint binary from mongodb's cache
    """
    # Get URL
    if platform == "Linux":
        url = ESLINT_HTTP_LINUX_CACHE
    elif platform == "Darwin":
        url = ESLINT_HTTP_DARWIN_CACHE
    else:
        raise ValueError('ESLint is not available as a binary for ' + platform)

    dest_dir = tempfile.gettempdir()
    temp_tar_file = os.path.join(dest_dir, "temp.tar.gz")

    # Download the file
    print("Downloading ESLint %s from %s, saving to %s" % (ESLINT_VERSION,
                                                           url, temp_tar_file))
    urllib.urlretrieve(url, temp_tar_file)

    eslint_distfile = ESLINT_SOURCE_TAR_BASE.substitute(platform=platform, arch=arch)
    extract_eslint(temp_tar_file, eslint_distfile)
    shutil.move(eslint_distfile, dest_file)


class ESLint(object):
    """Class encapsulates finding a suitable copy of ESLint, and linting an individual file
    """
    def __init__(self, path, cache_dir):
        eslint_progname = ESLINT_PROGNAME

        # Initialize ESLint configuration information
        if sys.platform.startswith("linux"):
            self.arch = "x86_64"
            self.tar_path = None
        elif sys.platform == "darwin":
            self.arch = "x86_64"
            self.tar_path = None

        self.path = None

        # Find ESLint now
        if path is not None:
            if os.path.isfile(path):
                self.path = path
            else:
                print("WARNING: Could not find ESLint at %s" % (path))

        # Check the environment variable
        if "MONGO_ESLINT" in os.environ:
            self.path = os.environ["MONGO_ESLINT"]

            if self.path and not self._validate_version(warn=True):
                self.path = None

        # Check the user's PATH environment variable now
        if self.path is None:
            self.path = spawn.find_executable(eslint_progname)

            if self.path and not self._validate_version(warn=True):
                self.path = None

        # Have not found it yet, download it from the web
        if self.path is None:
            if not os.path.isdir(cache_dir):
                os.makedirs(cache_dir)

            self.path = os.path.join(cache_dir, eslint_progname)

            if not os.path.isfile(self.path):
                if sys.platform.startswith("linux"):
                    get_eslint_from_cache(self.path, "Linux", self.arch)
                elif sys.platform == "darwin":
                    get_eslint_from_cache(self.path, "Darwin", self.arch)
                else:
                    print("ERROR: eslint.py does not support downloading ESLint " +
                          "on this platform, please install ESLint " + ESLINT_VERSION)
        # Validate we have the correct version
        if not self._validate_version():
            raise ValueError('correct version of ESLint was not found.')

        self.print_lock = threading.Lock()

    def _validate_version(self, warn=False):
        """Validate ESLint is the expected version
        """
        esl_version = callo([self.path, "--version"]).rstrip()
        # Ignore the leading v in the version string.
        if ESLINT_VERSION == esl_version[1:]:
            return True

        if warn:
            print("WARNING: eslint found in path, but incorrect version found at " +
                  self.path + " with version: " + esl_version)
        return False

    def _lint(self, file_name, print_diff):
        """Check the specified file for linting errors
        """
        # ESLint returns non-zero on a linting error. That's all we care about
        # so only enter the printing logic if we have an error.
        try:
            eslint_output = callo([self.path, "-f", "unix", file_name])
        except CalledProcessError as e:
            if print_diff:
                # Take a lock to ensure error messages do not get mixed when printed to the screen
                with self.print_lock:
                    print("ERROR: ESLint found errors in " + file_name)
                    print(e.output)
            return False
        except:
            print("ERROR: ESLint process threw unexpected error", sys.exc_info()[0])
            return False

        return True

    def lint(self, file_name):
        """Check the specified file has no linting errors
        """
        return self._lint(file_name, print_diff=True)

    def autofix(self, file_name):
        """ Run ESLint in fix mode.
        """
        return not subprocess.call([self.path, "--fix", file_name])

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
    """Get a list of linked repos and directories to run ESLint on.
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
        to run ESLint.
    """
    def __init__(self, path):
        self.path = path

        # Get candidate files
        self.candidate_files = self.get_candidate_files()

        self.root = self._get_root()

    def _callgito(self, args):
        """Call git for this repository
        """
        # These two flags are the equivalent of -C in newer versions of Git
        # but we use these to support versions back to ~1.8
        return callo(['git', '--git-dir', os.path.join(self.path, ".git"),
                        '--work-tree', self.path] + args)

    def _get_local_dir(self, path):
        """Get a directory path relative to the git root directory
        """
        if os.path.isabs(path):
            return os.path.relpath(path, self.root)
        return path

    def get_candidates(self, candidates):
        """Get the set of candidate files to check by doing an intersection
        between the input list, and the list of candidates in the repository

        Returns the full path to the files for ESLint to consume.
        """
        # NOTE: Files may have an absolute root (i.e. leading /)

        if candidates is not None and len(candidates) > 0:
            candidates = [self._get_local_dir(f) for f in candidates]
            valid_files = list(set(candidates).intersection(self.get_candidate_files()))
        else:
            valid_files = list(self.get_candidate_files())

        # Get the full file names here
        valid_files = [os.path.normpath(os.path.join(self.root, f)) for f in valid_files]
        return valid_files

    def _get_root(self):
        """Gets the root directory for this repository from git
        """
        gito = self._callgito(['rev-parse', '--show-toplevel'])

        return gito.rstrip()

    def get_candidate_files(self):
        """Query git to get a list of all files in the repo to consider for analysis
        """
        gito = self._callgito(["ls-files"])

        # This allows us to pick all the interesting files
        # in the mongo and mongo-enterprise repos
        file_list = [line.rstrip()
                     for line in gito.splitlines()
                     if "src/mongo" in line or "jstests" in line]

        files_match = re.compile('\\.js$')

        file_list = [a for a in file_list if files_match.search(a)]

        return file_list


def expand_file_string(glob_pattern):
    """Expand a string that represents a set of files
    """
    return [os.path.abspath(f) for f in globstar.iglob(glob_pattern)]

def get_files_to_check(files):
    """Filter the specified list of files to check down to the actual
        list of files that need to be checked."""
    candidates = []

    # Get a list of candidate_files
    candidates = [expand_file_string(f) for f in files]
    candidates = list(itertools.chain.from_iterable(candidates))

    repos = get_repos()

    valid_files = list(itertools.chain.from_iterable([r.get_candidates(candidates) for r in repos]))

    return valid_files

def get_files_to_check_from_patch(patches):
    """Take a patch file generated by git diff, and scan the patch for a list of files to check.
    """
    candidates = []

    # Get a list of candidate_files
    check = re.compile(r"^diff --git a\/([\w\/\.\-]+) b\/[\w\/\.\-]+")

    lines = []
    for patch in patches:
        with open(patch, "rb") as infile:
            lines += infile.readlines()

    candidates = [check.match(line).group(1) for line in lines if check.match(line)]

    repos = get_repos()

    valid_files = list(itertools.chain.from_iterable([r.get_candidates(candidates) for r in repos]))

    return valid_files

def _get_build_dir():
    """Get the location of the scons build directory in case we need to download ESLint
    """
    return os.path.join(get_base_dir(), "build")

def _lint_files(eslint, files):
    """Lint a list of files with ESLint
    """
    eslint = ESLint(eslint, _get_build_dir())

    lint_clean = parallel_process([os.path.abspath(f) for f in files], eslint.lint)

    if not lint_clean:
        print("ERROR: ESLint found errors. Run ESLint manually to see errors in "\
              "files that were skipped")
        sys.exit(1)

    return True

def lint_patch(eslint, infile):
    """Lint patch command entry point
    """
    files = get_files_to_check_from_patch(infile)

    # Patch may have files that we do not want to check which is fine
    if files:
        return _lint_files(eslint, files)
    return True

def lint(eslint, dirmode, glob):
    """Lint files command entry point
    """
    if dirmode and glob:
        files = glob
    else:
        files = get_files_to_check(glob)

    _lint_files(eslint, files)

    return True

def _autofix_files(eslint, files):
    """Auto-fix the specified files with ESLint.
    """
    eslint = ESLint(eslint, _get_build_dir())

    autofix_clean = parallel_process([os.path.abspath(f) for f in files], eslint.autofix)

    if not autofix_clean:
        print("ERROR: failed to auto-fix files")
        return False

def autofix_func(eslint, dirmode, glob):
    """Auto-fix files command entry point
    """
    if dirmode:
        files = glob
    else:
        files = get_files_to_check(glob)

    return _autofix_files(eslint, files)


def main():
    """Main entry point
    """
    success = False
    usage = "%prog [-e <eslint>] [-d] lint|lint-patch|fix [glob patterns] "
    description = "lint runs ESLint on provided patterns or all .js files under jstests/ "\
                  "and src/mongo. lint-patch runs ESLint against .js files modified in the "\
                  "provided patch file (for upload.py). "\
                  "fix runs ESLint with --fix on provided patterns "\
                  "or files under jstests/ and src/mongo."
    epilog ="*Unless you specify -d a separate ESLint process will be launched for every file"
    parser = OptionParser()
    parser = OptionParser(usage=usage, description=description, epilog=epilog)
    parser.add_option("-e", "--eslint", type="string", dest="eslint",
                      help="Fully qualified path to eslint executable",)
    parser.add_option("-d", "--dirmode", action="store_true", default=True, dest="dirmode",
                      help="Considers the glob patterns as directories and runs ESLint process " \
                           "against each pattern",)

    (options, args) = parser.parse_args(args=sys.argv)

    if len(args) > 1:
        command = args[1]
        searchlist = args[2:]
        if not searchlist:
            searchlist = ["jstests/", "src/mongo/"]

        if command == "lint":
            success = lint(options.eslint, options.dirmode, searchlist)
        elif command == "lint-patch":
            if not args[2:]:
                success = False
                print("You must provide the patch's fully qualified file name with lint-patch")
            else:
                success = lint_patch(options.eslint, searchlist)
        elif command == "fix":
            success = autofix_func(options.eslint, options.dirmode, searchlist)
        else:
            parser.print_help()
    else:
        parser.print_help()

    sys.exit(0 if success else 1)
if __name__ == "__main__":
    main()
