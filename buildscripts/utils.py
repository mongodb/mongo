"""Various utilities that are handy."""

import codecs
import os
import os.path
import re
import subprocess
import sys


def get_git_branch():
    """Return the git branch version."""
    if not os.path.exists(".git") or not os.path.isdir(".git"):
        return None

    version = open(".git/HEAD", "r").read().strip()
    if not version.startswith("ref: "):
        return version
    version = version.split("/")
    version = version[len(version) - 1]
    return version


def get_git_branch_string(prefix="", postfix=""):
    """Return the git branch name."""
    tt = re.compile(r"[/\\]").split(os.getcwd())
    if len(tt) > 2 and tt[len(tt) - 1] == "mongo":
        par = tt[len(tt) - 2]
        mt = re.compile(r".*_([vV]\d+\.\d+)$").match(par)
        if mt is not None:
            return prefix + mt.group(1).lower() + postfix
        if par.find("Nightly") > 0:
            return ""

    branch = get_git_branch()
    if branch is None or branch == "master":
        return ""
    return prefix + branch + postfix


def get_git_version():
    """Return the git version."""
    if not os.path.exists(".git") or not os.path.isdir(".git"):
        return "nogitversion"

    version = open(".git/HEAD", "r").read().strip()
    if not version.startswith("ref: "):
        return version
    version = version[5:]
    git_ver = ".git/" + version
    if not os.path.exists(git_ver):
        return version
    return open(git_ver, "r").read().strip()


def get_git_describe():
    """Return 'git describe --abbrev=7'."""
    with open(os.devnull, "r+") as devnull:
        proc = subprocess.Popen(
            "git describe --abbrev=7",
            stdout=subprocess.PIPE,
            stderr=devnull,
            stdin=devnull,
            shell=True,
        )
        return proc.communicate()[0].strip().decode("utf-8")


def execsys(args):
    """Execute a subprocess of 'args'."""
    if isinstance(args, str):
        rc = re.compile(r"\s+")
        args = rc.split(args)
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    res = proc.communicate()
    return res


def which(executable):
    """Return full path of 'executable'."""
    if sys.platform == "win32":
        paths = os.environ.get("Path", "").split(";")
    else:
        paths = os.environ.get("PATH", "").split(":")

    for path in paths:
        path = os.path.expandvars(path)
        path = os.path.expanduser(path)
        path = os.path.abspath(path)
        executable_path = os.path.join(path, executable)
        if os.path.exists(executable_path):
            return executable_path

    return executable


def replace_with_repr(unicode_error):
    """Codec error handler replacement."""
    # Unicode is a pain, some strings cannot be unicode()'d
    # but we want to just preserve the bytes in a human-readable
    # fashion. This codec error handler will substitute the
    # repr() of the offending bytes into the decoded string
    # at the position they occurred
    offender = unicode_error.object[unicode_error.start : unicode_error.end]
    return (str(repr(offender).strip("'").strip('"')), unicode_error.end)


codecs.register_error("repr", replace_with_repr)
