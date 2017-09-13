"""Module to run git commands on a repository."""

from __future__ import absolute_import

import logging
import os
import sys

# The subprocess32 module resolves the thread-safety issues of the subprocess module in Python 2.x
# when the _posixsubprocess C extension module is also available. Additionally, the _posixsubprocess
# C extension module avoids triggering invalid free() calls on Python's internal data structure for
# thread-local storage by skipping the PyOS_AfterFork() call when the 'preexec_fn' parameter isn't
# specified to subprocess.Popen(). See SERVER-22219 for more details.
#
# The subprocess32 module is untested on Windows and thus isn't recommended for use, even when it's
# installed. See https://github.com/google/python-subprocess32/blob/3.2.7/README.md#usage.
if os.name == "posix" and sys.version_info[0] == 2:
    try:
        import subprocess32 as subprocess
    except ImportError:
        import warnings
        warnings.warn(("Falling back to using the subprocess module because subprocess32 isn't"
                       " available. When using the subprocess module, a child process may trigger"
                       " an invalid free(). See SERVER-22219 for more details."),
                      RuntimeWarning)
        import subprocess
else:
    import subprocess


LOGGER = logging.getLogger(__name__)


class Repository(object):
    """Represent a local git repository."""
    def __init__(self, directory):
        self.directory = directory

    def git_add(self, args):
        """Run a git add command."""
        return self._callgito("add", args)

    def git_cat_file(self, args):
        """Run a git cat-file command."""
        return self._callgito("cat-file", args)

    def git_commit(self, args):
        """Run a git commit command."""
        return self._callgito("commit", args)

    def git_diff(self, args):
        """Run a git diff command."""
        return self._callgito("diff", args)

    def git_log(self, args):
        """Run a git log command."""
        return self._callgito("log", args)

    def git_push(self, args):
        """Run a git push command."""
        return self._callgito("push", args)

    def git_fetch(self, args):
        """Run a git fetch command."""
        return self._callgito("fetch", args)

    def git_ls_files(self, args):
        """Run a git ls-files command and return the result as a str."""
        return self._callgito("ls-files", args)

    def git_rebase(self, args):
        """Run a git rebase command."""
        return self._callgito("rebase", args)

    def git_reset(self, args):
        """Run a git reset command."""
        return self._callgito("reset", args)

    def git_rev_list(self, args):
        """Run a git rev-list command."""
        return self._callgito("rev-list", args)

    def git_rev_parse(self, args):
        """Run a git rev-parse command."""
        return self._callgito("rev-parse", args).rstrip()

    def git_rm(self, args):
        """Run a git rm command."""
        return self._callgito("rm", args)

    def git_show(self, args):
        """Run a git show command."""
        return self._callgito("show", args)

    def get_origin_url(self):
        """Return the URL of the origin repository."""
        return self._callgito(
            "config", ["--local", "--get", "remote.origin.url"]).rstrip()

    def get_branch_name(self):
        """
        Get the current branch name, short form.

        This returns "master", not "refs/head/master".
        Raises a GitException if the current branch is detached.
        """
        branch = self.git_rev_parse(["--abbrev-ref", "HEAD"])
        if branch == "HEAD":
            raise GitException("Branch is currently detached")
        return branch

    def get_current_revision(self):
        """Retrieve the current revision of the repository."""
        return self.git_rev_parse(["HEAD"]).rstrip()

    def configure(self, parameter, value):
        """Set a local configuration parameter."""
        return self._callgito("config", ["--local", parameter, value])

    def is_detached(self):
        """Return True if the current working tree in a detached HEAD state."""
        # symbolic-ref returns 1 if the repo is in a detached HEAD state
        return self._callgit("symbolic-ref", ["--quiet", "HEAD"]) == 1

    def is_ancestor(self, parent_revision, child_revision):
        """Return True if the specified parent hash an ancestor of child hash."""
        # If the common point between parent_revision and child_revision is
        # parent_revision, then parent_revision is an ancestor of child_revision.
        merge_base = self._callgito("merge-base", [parent_revision,
                                                   child_revision]).rstrip()
        return parent_revision == merge_base

    def is_commit(self, revision):
        """Return True if the specified hash is a valid git commit."""
        # cat-file -e returns 0 if it is a valid hash
        return not self._callgit("cat-file", ["-e", "{0}^{{commit}}".format(revision)])

    def is_working_tree_dirty(self):
        """Return True if the current working tree has changes."""
        # diff returns 1 if the working tree has local changes
        return self._callgit("diff", ["--quiet"]) == 1

    def does_branch_exist(self, branch):
        """Return True if the branch exists."""
        # rev-parse returns 0 if the branch exists
        return not self._callgit("rev-parse", ["--verify", branch])

    def get_merge_base(self, commit):
        """Get the merge base between 'commit' and HEAD."""
        return self._callgito("merge-base", ["HEAD", commit]).rstrip()

    def commit_with_message(self, message):
        """Commit the staged changes with the given message."""
        return self.git_commit(["--message", message])

    def push_to_remote_branch(self, remote, remote_branch):
        """Push the current branch to the specified remote repository and branch."""
        refspec = "{}:{}".format(self.get_branch_name(), remote_branch)
        return self.git_push([remote, refspec])

    def fetch_remote_branch(self, repository, branch):
        """Fetch the changes from a remote branch."""
        return self.git_fetch([repository, branch])

    def rebase_from_upstream(self, upstream, ignore_date=False):
        """Rebase the repository on an upstream reference.

        If 'ignore_date' is True, the '--ignore-date' option is passed to git.
        """
        args = [upstream]
        if ignore_date:
            args.append("--ignore-date")
        return self.git_rebase(args)

    @staticmethod
    def clone(url, directory, branch=None, depth=None):
        """Clone the repository designed by 'url' into 'directory'.

        Return a Repository instance."""
        params = ["git", "clone"]
        if branch:
            params += ["--branch", branch]
        if depth:
            params += ["--depth", depth]
        params += [url, directory]
        result = Repository._run_process("clone", params)
        result.check_returncode()
        return Repository(directory)

    @staticmethod
    def get_base_directory(directory=None):
        """Return the base directory of the repository the given directory belongs to.

        If no directory is specified, then the current working directory is used."""
        if directory is not None:
            params = ["git", "-C", directory]
        else:
            params = ["git"]
        params.extend(["rev-parse", "--show-toplevel"])
        result = Repository._run_process("rev-parse", params)
        result.check_returncode()
        return result.stdout.rstrip()

    @staticmethod
    def current_repository():
        """Return the Repository the current working directory belongs to."""
        return Repository(Repository.get_base_directory())

    def _callgito(self, cmd, args):
        """Call git for this repository, and return the captured output."""
        result = self._run_cmd(cmd, args)
        result.check_returncode()
        return result.stdout

    def _callgit(self, cmd, args, raise_exception=False):
        """
        Call git for this repository without capturing output.

        This is designed to be used when git returns non-zero exit codes.
        """
        result = self._run_cmd(cmd, args)
        if raise_exception:
            result.check_returncode()
        return result.returncode

    def _run_cmd(self, cmd, args):
        """Run the git command and return a GitCommandResult instance.
        """

        params = ["git", cmd] + args
        return self._run_process(cmd, params, cwd=self.directory)

    @staticmethod
    def _run_process(cmd, params, cwd=None):
        process = subprocess.Popen(params, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd)
        (stdout, stderr) = process.communicate()
        if process.returncode:
            if stdout:
                LOGGER.error("Output of '%s': %s", " ".join(params), stdout)
            if stderr:
                LOGGER.error("Error output of '%s': %s", " ".join(params), stderr)
        return GitCommandResult(cmd, params, process.returncode, stdout=stdout, stderr=stderr)


class GitException(Exception):
    """Custom Exception for the git module.

    Args:
        message: the exception message.
        returncode: the return code of the failed git command, if any.
        cmd: the git subcommand that was run, if any.
        process_args: a list containing the git command and arguments (includes 'git' as its first
            element) that were run, if any.
        stderr: the error output of the git command.
    """
    def __init__(self, message, returncode=None, cmd=None, process_args=None,
                 stdout=None, stderr=None):
        Exception.__init__(self, message)
        self.returncode = returncode
        self.cmd = cmd
        self.process_args = process_args
        self.stdout = stdout
        self.stderr = stderr


class GitCommandResult(object):
    """The result of running git subcommand.

    Args:
        cmd: the git subcommand that was executed (e.g. 'clone', 'diff').
        process_args: the full list of process arguments, starting with the 'git' command.
        returncode: the return code.
        stdout: the output of the command.
        stderr: the error output of the command.
    """

    def __init__(self, cmd, process_args, returncode, stdout=None, stderr=None):
        self.cmd = cmd
        self.process_args = process_args
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr

    def check_returncode(self):
        """Raise GitException if the exit code is non-zero."""
        if self.returncode:
            raise GitException(
                "Command '{0}' failed with code '{1}'".format(" ".join(self.process_args),
                                                              self.returncode),
                self.returncode, self.cmd, self.process_args, self.stdout, self.stderr)
