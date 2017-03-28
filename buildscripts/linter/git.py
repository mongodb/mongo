from __future__ import absolute_import
from __future__ import print_function

import itertools
import os
import re
import subprocess

from buildscripts import moduleconfig
from buildscripts.resmokelib.utils import globstar

# Path to the modules in the mongodb source tree
# Has to match the string in SConstruct
MODULE_DIR = "src/mongo/db/modules"

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

    def _callgito(self, args):
        """Call git for this repository, and return the captured output
        """
        # These two flags are the equivalent of -C in newer versions of Git
        # but we use these to support versions pre 1.8.5 but it depends on the command
        # and what the current directory is
        if "ls-files" in args:
            # This command depends on the current directory and works better if not run with
            # work-tree
            return subprocess.check_output(['git', '--git-dir', os.path.join(self.path, ".git")]
                                            + args)
        else:
            return subprocess.check_output(['git', '--git-dir', os.path.join(self.path, ".git"),
                                '--work-tree', self.path] + args)

    def _callgit(self, args):
        """Call git for this repository without capturing output
        This is designed to be used when git returns non-zero exit codes.
        """
        # These two flags are the equivalent of -C in newer versions of Git
        # but we use these to support versions pre 1.8.5 but it depends on the command
        # and what the current directory is
        return subprocess.call(['git', '--git-dir', os.path.join(self.path, ".git"),
                                '--work-tree', self.path] + args)

    def _get_local_dir(self, path):
        """Get a directory path relative to the git root directory
        """
        if os.path.isabs(path):
            path = os.path.relpath(path, self.path)

        # Normalize Windows style paths to Unix style which git uses on all platforms
        path = path.replace("\\", "/")

        return path

    def get_candidates(self, candidates, filter_function):
        """Get the set of candidate files to check by querying the repository

        Returns the full path to the file for clang-format to consume.
        """
        if candidates is not None and len(candidates) > 0:
            candidates = [self._get_local_dir(f) for f in candidates]
            valid_files = list(set(candidates).intersection(self.get_candidate_files(filter_function)))
        else:
            valid_files = list(self.get_candidate_files(filter_function))

        # Get the full file name here
        valid_files = [os.path.normpath(os.path.join(self.path, f)) for f in valid_files]

        return valid_files

    def _git_ls_files(self, cmd, filter_function):
        """Run git-ls-files and filter the list of files to a valid candidate list
        """
        gito = self._callgito(cmd)

        # This allows us to pick all the interesting files
        # in the mongo and mongo-enterprise repos
        file_list = [line.rstrip()
                for line in gito.splitlines() if filter_function(line.rstrip())]

        return file_list

    def get_candidate_files(self, filter_function):
        """Query git to get a list of all files in the repo to consider for analysis
        """
        return self._git_ls_files(["ls-files", "--cached"], filter_function)

    def get_working_tree_candidate_files(self, filter_function):
        """Query git to get a list of all files in the working tree to consider for analysis
        """
        return self._git_ls_files(["ls-files", "--cached", "--others"], filter_function)

    def get_working_tree_candidates(self, filter_function):
        """Get the set of candidate files to check by querying the repository

        Returns the full path to the file for clang-format to consume.
        """
        valid_files = list(self.get_working_tree_candidate_files(filter_function))

        # Get the full file name here
        valid_files = [os.path.normpath(os.path.join(self.path, f)) for f in valid_files]

        # Filter out files that git thinks exist but were removed.
        valid_files = [f for f in valid_files if os.path.exists(f)]

        return valid_files

    def is_detached(self):
        """Is the current working tree in a detached HEAD state?
        """
        # symbolic-ref returns 1 if the repo is in a detached HEAD state
        return self._callgit(["symbolic-ref", "--quiet", "HEAD"])

    def is_ancestor(self, parent, child):
        """Is the specified parent hash an ancestor of child hash?
        """
        # merge base returns 0 if parent is an ancestor of child
        return not self._callgit(["merge-base", "--is-ancestor", parent, child])

    def is_commit(self, sha1):
        """Is the specified hash a valid git commit?
        """
        # cat-file -e returns 0 if it is a valid hash
        return not self._callgit(["cat-file", "-e", "%s^{commit}" % sha1])

    def is_working_tree_dirty(self):
        """Does the current working tree have changes?
        """
        # diff returns 1 if the working tree has local changes
        return self._callgit(["diff", "--quiet"])

    def does_branch_exist(self, branch):
        """Does the branch exist?
        """
        # rev-parse returns 0 if the branch exists
        return not self._callgit(["rev-parse", "--verify", branch])

    def get_merge_base(self, commit):
        """Get the merge base between 'commit' and HEAD
        """
        return self._callgito(["merge-base", "HEAD", commit]).rstrip()

    def get_branch_name(self):
        """Get the current branch name, short form
           This returns "master", not "refs/head/master"
           Will not work if the current branch is detached
        """
        branch = self.rev_parse(["--abbrev-ref", "HEAD"])
        if branch == "HEAD":
            raise ValueError("Branch is currently detached")

        return branch

    def add(self, command):
        """git add wrapper
        """
        return self._callgito(["add"] + command)

    def checkout(self, command):
        """git checkout wrapper
        """
        return self._callgito(["checkout"] + command)

    def commit(self, command):
        """git commit wrapper
        """
        return self._callgito(["commit"] + command)

    def diff(self, command):
        """git diff wrapper
        """
        return self._callgito(["diff"] + command)

    def log(self, command):
        """git log wrapper
        """
        return self._callgito(["log"] + command)

    def rev_parse(self, command):
        """git rev-parse wrapper
        """
        return self._callgito(["rev-parse"] + command).rstrip()

    def rm(self, command):
        """git rm wrapper
        """
        return self._callgito(["rm"] + command)

    def show(self, command):
        """git show wrapper
        """
        return self._callgito(["show"] + command)


def expand_file_string(glob_pattern):
    """Expand a string that represents a set of files
    """
    return [os.path.abspath(f) for f in globstar.iglob(glob_pattern)]

def get_files_to_check_working_tree(filter_function):
    """Get a list of files to check from the working tree.
       This will pick up files not managed by git.
    """
    repos = get_repos()

    valid_files = list(itertools.chain.from_iterable([r.get_working_tree_candidates(filter_function) for r in repos]))

    return valid_files

def get_files_to_check(files, filter_function):
    """Get a list of files that need to be checked
       based on which files are managed by git.
    """
    # Get a list of candidate_files
    candidates = [expand_file_string(f) for f in files]
    candidates = list(itertools.chain.from_iterable(candidates))

    if len(files) > 0 and len(candidates) == 0:
        raise ValueError("Globs '%s' did not find any files." % (files))

    repos = get_repos()

    valid_files = list(itertools.chain.from_iterable([r.get_candidates(candidates, filter_function) for r in repos]))

    return valid_files

def get_files_to_check_from_patch(patches, filter_function):
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

    valid_files = list(itertools.chain.from_iterable([r.get_candidates(candidates, filter_function) for r in repos]))

    return valid_files
