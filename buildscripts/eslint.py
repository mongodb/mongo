#!/usr/bin/env python3
"""ESLint module.

Will download a prebuilt ESLint binary if necessary (i.e. it isn't installed, isn't in the current
path, or is the wrong version). It works in much the same way as clang_format.py. In lint mode, it
will lint the files or directory paths passed. In lint-patch mode, for upload.py, it will see if
there are any candidate files in the supplied patch. Fix mode will run ESLint with the --fix
option, and that will update the files with missing semicolons and similar repairable issues.
There is also a -d mode that assumes you only want to run one copy of ESLint per file / directory
parameter supplied. This lets ESLint search for candidate files to lint.
"""

import logging
import os
import shutil
import string
import subprocess
import sys
import tarfile
import tempfile
import threading
from typing import Optional
import urllib.error
import urllib.parse
import urllib.request

from distutils import spawn
from optparse import OptionParser
import structlog

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

# pylint: disable=wrong-import-position
from buildscripts.linter.filediff import gather_changed_files_for_lint
from buildscripts.linter import git, parallel
# pylint: enable=wrong-import-position

##############################################################################
#
# Constants for ESLint
#
#

# Expected version of ESLint.
# If you want to update the version, please refer to `buildscripts/eslint/README.md`
ESLINT_VERSION = "7.22.0"

# Name of ESLint as a binary.
ESLINT_PROGNAME = "eslint"

# URL location of our provided ESLint binaries.
ESLINT_HTTP_LINUX_CACHE = "https://s3.amazonaws.com/boxes.10gen.com/build/eslint-" + \
                           ESLINT_VERSION + "-linux.tar.gz"
ESLINT_HTTP_DARWIN_CACHE = "https://s3.amazonaws.com/boxes.10gen.com/build/eslint-" + \
                            ESLINT_VERSION + "-darwin.tar.gz"

# Path in the tarball to the ESLint binary.
ESLINT_SOURCE_TAR_BASE = string.Template(ESLINT_PROGNAME + "-$platform-$arch")

LOGGER = structlog.get_logger(__name__)


def callo(args):
    """Call a program, and capture its output."""
    return subprocess.check_output(args).decode('utf-8')


def extract_eslint(tar_path, target_file):
    """Extract ESLint tar file."""
    tarfp = tarfile.open(tar_path)
    for name in tarfp.getnames():
        if name == target_file:
            tarfp.extract(name)
    tarfp.close()


def get_eslint_from_cache(dest_file, platform, arch):
    """Get ESLint binary from mongodb's cache."""
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
    print("Downloading ESLint %s from %s, saving to %s" % (ESLINT_VERSION, url, temp_tar_file))
    urllib.request.urlretrieve(url, temp_tar_file)

    print("Extracting ESLint %s to %s" % (ESLINT_VERSION, dest_file))
    eslint_distfile = ESLINT_SOURCE_TAR_BASE.substitute(platform=platform, arch=arch)
    extract_eslint(temp_tar_file, eslint_distfile)
    shutil.move(eslint_distfile, dest_file)


class ESLint(object):
    """Class encapsulates finding a suitable copy of ESLint, and linting an individual file."""

    def __init__(self, path, cache_dir):
        """Initialize ESLint."""
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
                print("WARNING: Could not find ESLint at %s" % path)

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

            if os.path.isfile(self.path) and not self._validate_version(warn=True):
                print(
                    "WARNING: removing ESLint from %s to download the correct version" % self.path)
                os.remove(self.path)

            if not os.path.isfile(self.path):
                if sys.platform.startswith("linux"):
                    get_eslint_from_cache(self.path, "Linux", self.arch)
                elif sys.platform == "darwin":
                    get_eslint_from_cache(self.path, "Darwin", self.arch)
                else:
                    print("ERROR: eslint.py does not support downloading ESLint "
                          "on this platform, please install ESLint " + ESLINT_VERSION)
        # Validate we have the correct version
        if not self._validate_version():
            raise ValueError('correct version of ESLint was not found.')

        self.print_lock = threading.Lock()

    def _validate_version(self, warn=False):
        """Validate ESLint is the expected version."""
        esl_version = callo([self.path, "--version"]).rstrip()
        # Ignore the leading v in the version string.
        if ESLINT_VERSION == esl_version[1:]:
            return True

        if warn:
            print("WARNING: ESLint found in path %s, but the version is incorrect: %s" %
                  (self.path, esl_version))
        return False

    def _lint(self, file_name, print_diff):
        """Check the specified file for linting errors."""
        # ESLint returns non-zero on a linting error. That's all we care about
        # so only enter the printing logic if we have an error.
        try:
            callo([self.path, "-f", "unix", file_name])
        except subprocess.CalledProcessError as err:
            if print_diff:
                # Take a lock to ensure error messages do not get mixed when printed to the screen
                with self.print_lock:
                    print("ERROR: ESLint found errors in " + file_name)
                    print(err.output)
            return False

        return True

    def lint(self, file_name):
        """Check the specified file has no linting errors."""
        return self._lint(file_name, print_diff=True)

    def autofix(self, file_name):
        """Run ESLint in fix mode."""
        return not subprocess.call([self.path, "--fix", file_name])


def is_interesting_file(file_name):
    """Return true if this file should be checked."""
    return ((file_name.startswith("src/mongo") or file_name.startswith("jstests"))
            and file_name.endswith(".js"))


def _get_build_dir():
    """Get the location of the scons build directory in case we need to download ESLint."""
    return os.path.join(git.get_base_dir(), "build")


def _lint_files(eslint, files):
    """Lint a list of files with ESLint."""
    eslint = ESLint(eslint, _get_build_dir())

    print("Running ESLint %s at %s" % (ESLINT_VERSION, eslint.path))
    lint_clean = parallel.parallel_process([os.path.abspath(f) for f in files], eslint.lint)

    if not lint_clean:
        print("ERROR: ESLint found errors. Run ESLint manually to see errors in "
              "files that were skipped")
        sys.exit(1)

    return True


def lint_patch(eslint, infile):
    """Lint patch command entry point."""
    files = git.get_files_to_check_from_patch(infile, is_interesting_file)

    # Patch may have files that we do not want to check which is fine
    if files:
        return _lint_files(eslint, files)
    return True


def lint_git_diff(eslint: Optional[str]) -> bool:
    """
    Lint the files that have changes since the last git commit.

    :param eslint: Path to eslint command.
    :return: True if lint was successful.
    """
    files = gather_changed_files_for_lint(is_interesting_file)

    # Patch may have files that we do not want to check which is fine
    if files:
        return _lint_files(eslint, files)
    return True


def lint(eslint, dirmode, glob):
    """Lint files command entry point."""
    if dirmode and glob:
        files = glob
    else:
        files = git.get_files_to_check(glob, is_interesting_file)

    _lint_files(eslint, files)

    return True


def _autofix_files(eslint, files):
    """Auto-fix the specified files with ESLint."""
    eslint = ESLint(eslint, _get_build_dir())

    print("Running ESLint %s at %s" % (ESLINT_VERSION, eslint.path))
    autofix_clean = parallel.parallel_process([os.path.abspath(f) for f in files], eslint.autofix)

    if not autofix_clean:
        print("ERROR: failed to auto-fix files")
        return False
    return True


def autofix_func(eslint, dirmode, glob):
    """Auto-fix files command entry point."""
    if dirmode:
        files = glob
    else:
        files = git.get_files_to_check(glob, is_interesting_file)

    return _autofix_files(eslint, files)


def main():
    """Execute Main entry point."""
    success = False
    usage = "%prog [-e <eslint>] [-d] lint|lint-patch|fix [glob patterns] "
    description = ("The script will try to find ESLint version %s on your system and run it. "
                   "If it won't find the version it will try to download it and then run it. "
                   "Commands description: lint runs ESLint on provided patterns or all .js "
                   "files under `jstests/` and `src/mongo`; "
                   "lint-patch runs ESLint against .js files modified in the "
                   "provided patch file (for upload.py); "
                   "fix runs ESLint with --fix on provided patterns "
                   "or files under jstests/ and src/mongo." % ESLINT_VERSION)
    epilog = "*Unless you specify -d a separate ESLint process will be launched for every file"
    parser = OptionParser(usage=usage, description=description, epilog=epilog)
    parser.add_option(
        "-e",
        "--eslint",
        type="string",
        dest="eslint",
        help="Fully qualified path to eslint executable",
    )
    parser.add_option(
        "-d",
        "--dirmode",
        action="store_true",
        default=True,
        dest="dirmode",
        help="Considers the glob patterns as directories and runs ESLint process "
        "against each pattern",
    )

    (options, args) = parser.parse_args(args=sys.argv)

    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())

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
        elif command == "lint-git-diff":
            success = lint_git_diff(options.eslint)
        elif command == "fix":
            success = autofix_func(options.eslint, options.dirmode, searchlist)
        else:
            parser.print_help()
    else:
        parser.print_help()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
