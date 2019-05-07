"""Various utilities that are handy."""

import codecs
import re
import os
import os.path
import subprocess
import sys


def get_all_source_files(arr=None, prefix="."):
    """Return source files."""
    if arr is None:
        arr = []

    if not os.path.isdir(prefix):
        # assume a file
        arr.append(prefix)
        return arr

    for fx in os.listdir(prefix):
        # pylint: disable=too-many-boolean-expressions
        if (fx.startswith(".") or fx.startswith("pcre-") or fx.startswith("32bit")
                or fx.startswith("mongodb-") or fx.startswith("debian")
                or fx.startswith("mongo-cxx-driver") or fx.startswith("sqlite") or "gotools" in fx
                or fx.find("mozjs") != -1):
            continue
        # pylint: enable=too-many-boolean-expressions

        def is_followable_dir(prefix, full):
            """Return True if 'full' is a followable directory."""
            if not os.path.isdir(full):
                return False
            if not os.path.islink(full):
                return True
            # Follow softlinks in the modules directory (e.g: enterprise).
            if os.path.split(prefix)[1] == "modules":
                return True
            return False

        full = prefix + "/" + fx
        if is_followable_dir(prefix, full):
            get_all_source_files(arr, full)
        else:
            if full.endswith(".cpp") or full.endswith(".h") or full.endswith(".c"):
                full = full.replace("//", "/")
                arr.append(full)

    return arr


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
        proc = subprocess.Popen("git describe --abbrev=7", stdout=subprocess.PIPE, stderr=devnull,
                                stdin=devnull, shell=True)
        return proc.communicate()[0].strip().decode('utf-8')


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


def find_python(min_version=(3, 7)):
    """Return path of python."""
    try:
        return sys.executable
    except AttributeError:
        # In case the version of Python is somehow missing sys.version_info or sys.executable.
        pass

    version = re.compile(r"[Pp]ython ([\d\.]+)", re.MULTILINE)
    binaries = ("python37", "python3.7", "python36", "python3.6", "python35", "python3.5", "python")
    for binary in binaries:
        try:
            out, err = subprocess.Popen([binary, "-V"], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE).communicate()
            for stream in (out, err):
                match = version.search(stream)
                if match:
                    versiontuple = tuple(map(int, match.group(1).split(".")))
                    if versiontuple >= min_version:
                        return which(binary)
        except Exception:  # pylint: disable=broad-except
            pass

    raise Exception(
        "could not find suitable Python (version >= %s)" % ".".join(str(v) for v in min_version))


def replace_with_repr(unicode_error):
    """Codec error handler replacement."""
    # Unicode is a pain, some strings cannot be unicode()'d
    # but we want to just preserve the bytes in a human-readable
    # fashion. This codec error handler will substitute the
    # repr() of the offending bytes into the decoded string
    # at the position they occurred
    offender = unicode_error.object[unicode_error.start:unicode_error.end]
    return (str(repr(offender).strip("'").strip('"')), unicode_error.end)


codecs.register_error("repr", replace_with_repr)
