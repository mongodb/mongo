"""Git Utility functions."""
from __future__ import absolute_import
from __future__ import print_function

import itertools
import os
import re
from typing import Any, Callable, List, Tuple

from buildscripts import git as _git
from buildscripts import moduleconfig
from buildscripts.resmokelib.utils import globstar

# Path to the modules in the mongodb source tree
# Has to match the string in SConstruct
MODULE_DIR = "src/mongo/db/modules"


def get_base_dir():
    # type: () -> str
    """
    Get the base directory for mongo repo.

    This script assumes that it is running in buildscripts/, and uses
    that to find the base directory.
    """
    try:
        return _git.Repository.get_base_directory()
    except _git.GitException:
        # We are not in a valid git directory. Use the script path instead.
        return os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


def get_repos():
    # type: () -> List[Repo]
    """Get a list of Repos to check linters for."""
    base_dir = get_base_dir()

    # Get a list of modules
    # TODO: how do we filter rocks, does it matter?
    mongo_modules = moduleconfig.discover_module_directories(
        os.path.join(base_dir, MODULE_DIR), None)

    paths = [os.path.join(base_dir, MODULE_DIR, m) for m in mongo_modules]

    paths.append(base_dir)

    return [Repo(p) for p in paths]


class Repo(_git.Repository):
    """Class encapsulates all knowledge about a git repository, and its metadata to run linters."""

    def _get_local_dir(self, path):
        # type: (str) -> str
        """Get a directory path relative to the git root directory."""
        if os.path.isabs(path):
            path = os.path.relpath(path, self.directory)

        # Normalize Windows style paths to Unix style which git uses on all platforms
        path = path.replace("\\", "/")

        return path

    def get_candidates(self, candidates, filter_function):
        # type: (List[str], Callable[[str], bool]) -> List[str]
        """
        Get the set of candidate files to check by querying the repository.

        Returns the full path to the file for clang-format to consume.
        """
        if candidates is not None and len(candidates) > 0:  # pylint: disable=len-as-condition
            candidates = [self._get_local_dir(f) for f in candidates]
            valid_files = list(
                set(candidates).intersection(self.get_candidate_files(filter_function)))
        else:
            valid_files = list(self.get_candidate_files(filter_function))

        # Get the full file name here
        valid_files = [os.path.normpath(os.path.join(self.directory, f)) for f in valid_files]

        return valid_files

    def _git_ls_files(self, args, filter_function):
        # type: (List[str], Callable[[str], bool]) -> List[str]
        """Run git-ls-files and filter the list of files to a valid candidate list."""
        gito = self.git_ls_files(args)

        # This allows us to pick all the interesting files
        # in the mongo and mongo-enterprise repos
        file_list = [line.rstrip() for line in gito.splitlines() if filter_function(line.rstrip())]

        return file_list

    def get_candidate_files(self, filter_function):
        # type: (Callable[[str], bool]) -> List[str]
        """Query git to get a list of all files in the repo to consider for analysis."""
        return self._git_ls_files(["--cached"], filter_function)

    def get_my_candidate_files(self, filter_function, origin_branch):
        # type: (Callable[[str], bool], str) -> List[str]
        """Query git to get a list of files in the repo from a diff."""
        # There are 3 diffs we run:
        # 1. List of commits between origin/master and HEAD of current branch
        # 2. Cached/Staged files (--cached)
        # 3. Working Tree files git tracks

        fork_point = self.get_merge_base(["HEAD", origin_branch])

        diff_files = self.git_diff(["--name-only", "%s..HEAD" % (fork_point)])
        diff_files += self.git_diff(["--name-only", "--cached"])
        diff_files += self.git_diff(["--name-only"])

        file_set = {
            os.path.normpath(os.path.join(self.directory, line.rstrip()))
            for line in diff_files.splitlines() if filter_function(line.rstrip())
        }

        return list(file_set)

    def get_working_tree_candidate_files(self, filter_function):
        # type: (Callable[[str], bool]) -> List[str]
        # pylint: disable=invalid-name
        """Query git to get a list of all files in the working tree to consider for analysis."""
        return self._git_ls_files(["--cached", "--others"], filter_function)

    def get_working_tree_candidates(self, filter_function):
        # type: (Callable[[str], bool]) -> List[str]
        """
        Get the set of candidate files to check by querying the repository.

        Returns the full path to the file for clang-format to consume.
        """
        valid_files = list(self.get_working_tree_candidate_files(filter_function))

        # Get the full file name here
        valid_files = [os.path.normpath(os.path.join(self.directory, f)) for f in valid_files]

        # Filter out files that git thinks exist but were removed.
        valid_files = [f for f in valid_files if os.path.exists(f)]

        return valid_files


def expand_file_string(glob_pattern):
    # type: (str) -> List[str]
    """Expand a string that represents a set of files."""
    return [os.path.abspath(f) for f in globstar.iglob(glob_pattern)]


def get_files_to_check_working_tree(filter_function):
    # type: (Callable[[str], bool]) -> List[str]
    """
    Get a list of files to check from the working tree.

    This will pick up files not managed by git.
    """
    repos = get_repos()

    valid_files = list(
        itertools.chain.from_iterable(
            [r.get_working_tree_candidates(filter_function) for r in repos]))

    return valid_files


def get_files_to_check(files, filter_function):
    # type: (List[str], Callable[[str], bool]) -> List[str]
    """Get a list of files that need to be checked based on which files are managed by git."""
    # Get a list of candidate_files
    candidates_nested = [expand_file_string(f) for f in files]
    candidates = list(itertools.chain.from_iterable(candidates_nested))

    if files and not candidates:
        raise ValueError("Globs '%s' did not find any files with glob." % (files))

    repos = get_repos()

    valid_files = list(
        itertools.chain.from_iterable(
            [r.get_candidates(candidates, filter_function) for r in repos]))

    if files and not valid_files:
        raise ValueError("Globs '%s' did not find any files with glob in git." % (files))

    return valid_files


def get_files_to_check_from_patch(patches, filter_function):
    # type: (List[str], Callable[[str], bool]) -> List[str]
    """Take a patch file generated by git diff, and scan the patch for a list of files to check."""
    candidates = []  # type: List[str]

    # Get a list of candidate_files
    check = re.compile(r"^diff --git a\/([\w\/\.\-]+) b\/[\w\/\.\-]+")

    lines = []  # type: List[str]
    for patch in patches:
        with open(patch, "rb") as infile:
            lines += infile.readlines()

    candidates = [check.match(line).group(1) for line in lines if check.match(line)]

    repos = get_repos()

    valid_files = list(
        itertools.chain.from_iterable(
            [r.get_candidates(candidates, filter_function) for r in repos]))

    return valid_files


def get_my_files_to_check(filter_function, origin_branch):
    # type: (Callable[[str], bool], str) -> List[str]
    """Get a list of files that need to be checked based on which files are managed by git."""
    # Get a list of candidate_files based on diff between this branch and origin/master
    repos = get_repos()

    valid_files = list(
        itertools.chain.from_iterable(
            [r.get_my_candidate_files(filter_function, origin_branch) for r in repos]))

    return valid_files
