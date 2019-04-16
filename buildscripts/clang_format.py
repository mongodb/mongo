#!/usr/bin/env python3
"""Clang format script that provides the following.

1. Ability to grab binaries where possible from LLVM.
2. Ability to download binaries from MongoDB cache for clang-format.
3. Validates clang-format is the right version.
4. Has support for checking which files are to be checked.
5. Supports validating and updating a set of files to the right coding style.
"""

import difflib
import glob
import os
import re
import shutil
import string
import subprocess
import sys
import tarfile
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request

from distutils import spawn  # pylint: disable=no-name-in-module
from optparse import OptionParser
from multiprocessing import cpu_count

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(os.path.realpath(__file__)))))

from buildscripts.linter import git  # pylint: disable=wrong-import-position
from buildscripts.linter import parallel  # pylint: disable=wrong-import-position

##############################################################################
#
# Constants for clang-format
#
#

# Expected version of clang-format
CLANG_FORMAT_VERSION = "3.8.0"
CLANG_FORMAT_SHORT_VERSION = "3.8"

# Name of clang-format as a binary
CLANG_FORMAT_PROGNAME = "clang-format"

# URL location of the "cached" copy of clang-format to download
# for users which do not have clang-format installed
CLANG_FORMAT_HTTP_LINUX_CACHE = "https://s3.amazonaws.com/boxes.10gen.com/build/clang-format-3.8-rhel55.tar.gz"

CLANG_FORMAT_HTTP_DARWIN_CACHE = "https://s3.amazonaws.com/boxes.10gen.com/build/clang%2Bllvm-3.8.0-x86_64-apple-darwin.tar.xz"

# Path in the tarball to the clang-format binary
CLANG_FORMAT_SOURCE_TAR_BASE = string.Template("clang+llvm-$version-$tar_path/bin/" +
                                               CLANG_FORMAT_PROGNAME)


##############################################################################
def callo(args):
    """Call a program, and capture its output."""
    return subprocess.check_output(args).decode('utf-8')


def get_tar_path(version, tar_path):
    """Return the path to clang-format in the llvm tarball."""
    return CLANG_FORMAT_SOURCE_TAR_BASE.substitute(version=version, tar_path=tar_path)


def extract_clang_format(tar_path):
    """Extract the clang_format tar file."""
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


def get_clang_format_from_cache_and_extract(url, tarball_ext):
    """Get clang-format from mongodb's cache and extract the tarball."""
    dest_dir = tempfile.gettempdir()
    temp_tar_file = os.path.join(dest_dir, "temp.tar" + tarball_ext)

    # Download from file
    print("Downloading clang-format %s from %s, saving to %s" % (CLANG_FORMAT_VERSION, url,
                                                                 temp_tar_file))

    # Retry download up to 5 times.
    num_tries = 5
    for attempt in range(num_tries):
        try:
            resp = urllib.request.urlopen(url)
            with open(temp_tar_file, 'wb') as fh:
                fh.write(resp.read())
            break
        except urllib.error.URLError:
            if attempt == num_tries - 1:
                raise
            continue

    extract_clang_format(temp_tar_file)


def get_clang_format_from_darwin_cache(dest_file):
    """Download clang-format from llvm.org, unpack the tarball to dest_file."""
    get_clang_format_from_cache_and_extract(CLANG_FORMAT_HTTP_DARWIN_CACHE, ".xz")

    # Destination Path
    shutil.move(get_tar_path(CLANG_FORMAT_VERSION, "x86_64-apple-darwin"), dest_file)


def get_clang_format_from_linux_cache(dest_file):
    """Get clang-format from mongodb's cache."""
    get_clang_format_from_cache_and_extract(CLANG_FORMAT_HTTP_LINUX_CACHE, ".gz")

    # Destination Path
    shutil.move("build/bin/clang-format", dest_file)


class ClangFormat(object):
    """ClangFormat class."""

    def __init__(self, path, cache_dir):  # pylint: disable=too-many-branches
        """Initialize ClangFormat."""
        self.path = None
        clang_format_progname_ext = ""

        if sys.platform == "win32":
            clang_format_progname_ext += ".exe"

        # Check the clang-format the user specified
        if path is not None:
            if os.path.isfile(path):
                self.path = path
            else:
                print("WARNING: Could not find clang-format %s" % (path))

        # Check the environment variable
        if "MONGO_CLANG_FORMAT" in os.environ:
            self.path = os.environ["MONGO_CLANG_FORMAT"]

            if self.path and not self._validate_version():
                self.path = None

        # Check the users' PATH environment variable now
        if self.path is None:
            # Check for various versions staring with binaries with version specific suffixes in the
            # user's path
            programs = [
                CLANG_FORMAT_PROGNAME + "-" + CLANG_FORMAT_VERSION,
                CLANG_FORMAT_PROGNAME + "-" + CLANG_FORMAT_SHORT_VERSION,
                CLANG_FORMAT_PROGNAME,
            ]

            if sys.platform == "win32":
                for i, _ in enumerate(programs):
                    programs[i] += '.exe'

            for program in programs:
                self.path = spawn.find_executable(program)

                if self.path:
                    if not self._validate_version():
                        self.path = None
                    else:
                        break

        # If Windows, try to grab it from Program Files
        # Check both native Program Files and WOW64 version
        if sys.platform == "win32":
            programfiles = [
                os.environ["ProgramFiles"],
                os.environ["ProgramFiles(x86)"],
            ]

            for programfile in programfiles:
                win32bin = os.path.join(programfile, "LLVM\\bin\\clang-format.exe")
                if os.path.exists(win32bin):
                    self.path = win32bin
                    break

        # Have not found it yet, download it from the web
        if self.path is None:
            if not os.path.isdir(cache_dir):
                os.makedirs(cache_dir)

            self.path = os.path.join(
                cache_dir,
                CLANG_FORMAT_PROGNAME + "-" + CLANG_FORMAT_VERSION + clang_format_progname_ext)

            # Download a new version if the cache is empty or stale
            if not os.path.isfile(self.path) or not self._validate_version():
                if sys.platform.startswith("linux"):
                    get_clang_format_from_linux_cache(self.path)
                elif sys.platform == "darwin":
                    get_clang_format_from_darwin_cache(self.path)
                else:
                    print("ERROR: clang-format.py does not support downloading clang-format " +
                          " on this platform, please install clang-format " + CLANG_FORMAT_VERSION)

        # Validate we have the correct version
        # We only can fail here if the user specified a clang-format binary and it is the wrong
        # version
        if not self._validate_version():
            print("ERROR: exiting because of previous warning.")
            sys.exit(1)

        self.print_lock = threading.Lock()

    def _validate_version(self):
        """Validate clang-format is the expected version."""
        cf_version = callo([self.path, "--version"])

        if CLANG_FORMAT_VERSION in cf_version:
            return True

        print("WARNING: clang-format found in path, but incorrect version found at " + self.path +
              " with version: " + cf_version)

        return False

    def _lint(self, file_name, print_diff):
        """Check the specified file has the correct format."""
        with open(file_name, 'rb') as original_text:
            original_file = original_text.read().decode('utf-8')

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
                    print("To fix formatting errors, run %s --style=file -i %s" % (self.path,
                                                                                   file_name))
                    for line in result:
                        print(line.rstrip())

            return False

        return True

    def lint(self, file_name):
        """Check the specified file has the correct format."""
        return self._lint(file_name, print_diff=True)

    def format(self, file_name):
        """Update the format of the specified file."""
        if self._lint(file_name, print_diff=False):
            return True

        # Update the file with clang-format
        formatted = not subprocess.call([self.path, "--style=file", "-i", file_name])

        # Version 3.8 generates files like foo.cpp~RF83372177.TMP when it formats foo.cpp
        # on Windows, we must clean these up
        if sys.platform == "win32":
            glob_pattern = file_name + "*.TMP"
            for fglob in glob.glob(glob_pattern):
                os.unlink(fglob)

        return formatted


FILES_RE = re.compile('\\.(h|hpp|ipp|cpp|js)$')


def is_interesting_file(file_name):
    """Return true if this file should be checked."""
    return ((file_name.startswith("jstests") or file_name.startswith("src"))
            and not file_name.startswith("src/third_party/")
            and not file_name.startswith("src/mongo/gotools/")) and FILES_RE.search(file_name)


def get_list_from_lines(lines):
    """Convert a string containing a series of lines into a list of strings."""
    return [line.rstrip() for line in lines.splitlines()]


def _get_build_dir():
    """Return the location of the scons' build directory."""
    return os.path.join(git.get_base_dir(), "build")


def _lint_files(clang_format, files):
    """Lint a list of files with clang-format."""
    clang_format = ClangFormat(clang_format, _get_build_dir())

    lint_clean = parallel.parallel_process([os.path.abspath(f) for f in files], clang_format.lint)

    if not lint_clean:
        print("ERROR: Code Style does not match coding style")
        sys.exit(1)


def lint_patch(clang_format, infile):
    """Lint patch command entry point."""
    files = git.get_files_to_check_from_patch(infile, is_interesting_file)

    # Patch may have files that we do not want to check which is fine
    if files:
        _lint_files(clang_format, files)


def lint(clang_format):
    """Lint files command entry point."""
    files = git.get_files_to_check([], is_interesting_file)

    _lint_files(clang_format, files)

    return True


def lint_all(clang_format):
    """Lint files command entry point based on working tree."""
    files = git.get_files_to_check_working_tree(is_interesting_file)

    _lint_files(clang_format, files)

    return True


def _format_files(clang_format, files):
    """Format a list of files with clang-format."""
    clang_format = ClangFormat(clang_format, _get_build_dir())

    format_clean = parallel.parallel_process([os.path.abspath(f) for f in files],
                                             clang_format.format)

    if not format_clean:
        print("ERROR: failed to format files")
        sys.exit(1)


def format_func(clang_format):
    """Format files command entry point."""
    files = git.get_files_to_check([], is_interesting_file)

    _format_files(clang_format, files)


def format_my_func(clang_format, origin_branch):
    """My Format files command entry point."""
    files = git.get_my_files_to_check(is_interesting_file, origin_branch)

    _format_files(clang_format, files)


def reformat_branch(  # pylint: disable=too-many-branches,too-many-locals,too-many-statements
        clang_format, commit_prior_to_reformat, commit_after_reformat):
    """Reformat a branch made before a clang-format run."""
    clang_format = ClangFormat(clang_format, _get_build_dir())

    if os.getcwd() != git.get_base_dir():
        raise ValueError("reformat-branch must be run from the repo root")

    if not os.path.exists("buildscripts/clang_format.py"):
        raise ValueError("reformat-branch is only supported in the mongo repo")

    repo = git.Repo(git.get_base_dir())

    # Validate that user passes valid commits
    if not repo.is_commit(commit_prior_to_reformat):
        raise ValueError("Commit Prior to Reformat '%s' is not a valid commit in this repo" %
                         commit_prior_to_reformat)

    if not repo.is_commit(commit_after_reformat):
        raise ValueError(
            "Commit After Reformat '%s' is not a valid commit in this repo" % commit_after_reformat)

    if not repo.is_ancestor(commit_prior_to_reformat, commit_after_reformat):
        raise ValueError(
            ("Commit Prior to Reformat '%s' is not a valid ancestor of Commit After" +
             " Reformat '%s' in this repo") % (commit_prior_to_reformat, commit_after_reformat))

    # Validate the user is on a local branch that has the right merge base
    if repo.is_detached():
        raise ValueError("You must not run this script in a detached HEAD state")

    # Validate the user has no pending changes
    if repo.is_working_tree_dirty():
        raise ValueError(
            "Your working tree has pending changes. You must have a clean working tree before proceeding."
        )

    merge_base = repo.get_merge_base(["HEAD", commit_prior_to_reformat])

    if not merge_base == commit_prior_to_reformat:
        raise ValueError(
            "Please rebase to '%s' and resolve all conflicts before running this script" %
            (commit_prior_to_reformat))

    # We assume the target branch is master, it could be a different branch if needed for testing
    merge_base = repo.get_merge_base(["HEAD", "master"])

    if not merge_base == commit_prior_to_reformat:
        raise ValueError(
            "This branch appears to already have advanced too far through the merge process")

    # Everything looks good so lets start going through all the commits
    branch_name = repo.get_branch_name()
    new_branch = "%s-reformatted" % branch_name

    if repo.does_branch_exist(new_branch):
        raise ValueError(
            "The branch '%s' already exists. Please delete the branch '%s', or rename the current branch."
            % (new_branch, new_branch))

    commits = get_list_from_lines(
        repo.log(["--reverse", "--pretty=format:%H",
                  "%s..HEAD" % commit_prior_to_reformat]))

    previous_commit_base = commit_after_reformat

    files_match = re.compile('\\.(h|cpp|js)$')

    # Go through all the commits the user made on the local branch and migrate to a new branch
    # that is based on post_reformat commits instead
    for commit_hash in commits:
        repo.checkout(["--quiet", commit_hash])

        deleted_files = []

        # Format each of the files by checking out just a single commit from the user's branch
        commit_files = get_list_from_lines(repo.diff(["HEAD~", "--name-only"]))

        for commit_file in commit_files:

            # Format each file needed if it was not deleted
            if not os.path.exists(commit_file):
                print("Skipping file '%s' since it has been deleted in commit '%s'" % (commit_file,
                                                                                       commit_hash))
                deleted_files.append(commit_file)
                continue

            if files_match.search(commit_file):
                clang_format.format(commit_file)
            else:
                print("Skipping file '%s' since it is not a file clang_format should format" %
                      commit_file)

        # Check if anything needed reformatting, and if so amend the commit
        if not repo.is_working_tree_dirty():
            print("Commit %s needed no reformatting" % commit_hash)
        else:
            repo.commit(["--all", "--amend", "--no-edit"])

        # Rebase our new commit on top the post-reformat commit
        previous_commit = repo.rev_parse(["HEAD"])

        # Checkout the new branch with the reformatted commits
        # Note: we will not name as a branch until we are done with all commits on the local branch
        repo.checkout(["--quiet", previous_commit_base])

        # Copy each file from the reformatted commit on top of the post reformat
        diff_files = get_list_from_lines(
            repo.diff(["%s~..%s" % (previous_commit, previous_commit), "--name-only"]))

        for diff_file in diff_files:
            # If the file was deleted in the commit we are reformatting, we need to delete it again
            if diff_file in deleted_files:
                repo.rm([diff_file])
                continue

            # The file has been added or modified, continue as normal
            file_contents = repo.show(["%s:%s" % (previous_commit, diff_file)])

            root_dir = os.path.dirname(diff_file)
            if root_dir and not os.path.exists(root_dir):
                os.makedirs(root_dir)

            with open(diff_file, "w+") as new_file:
                new_file.write(file_contents)

            repo.add([diff_file])

        # Create a new commit onto clang-formatted branch
        repo.commit(["--reuse-message=%s" % previous_commit])

        previous_commit_base = repo.rev_parse(["HEAD"])

    # Create a new branch to mark the hashes we have been using
    repo.checkout(["-b", new_branch])

    print("reformat-branch is done running.\n")
    print("A copy of your branch has been made named '%s', and formatted with clang-format.\n" %
          new_branch)
    print("The original branch has been left unchanged.")
    print("The next step is to rebase the new branch on 'master'.")


def usage():
    """Print usage."""
    print(
        "clang-format.py supports 6 commands [ lint, lint-all, lint-patch, format, format-my, reformat-branch]."
    )
    print("\nformat-my <origin branch>")
    print("   <origin branch>  - upstream branch to compare against")


def main():
    """Execute Main entry point."""
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
        elif command == "format-my":

            format_my_func(options.clang_format, args[2] if len(args) > 2 else "origin/master")
        elif command == "reformat-branch":

            if len(args) < 3:
                print(
                    "ERROR: reformat-branch takes two parameters: commit_prior_to_reformat commit_after_reformat"
                )
                return

            reformat_branch(options.clang_format, args[2], args[3])
        else:
            usage()
    else:
        usage()


if __name__ == "__main__":
    main()
