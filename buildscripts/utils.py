"""Various utilities that are handy."""

import codecs
import re
import os
import os.path
import subprocess
import sys


def getAllSourceFiles(arr=None, prefix="."):
    if arr is None:
        arr = []

    if not os.path.isdir(prefix):
        # assume a file
        arr.append(prefix)
        return arr

    for x in os.listdir(prefix):
        if (x.startswith(".")
            or x.startswith("pcre-")
            or x.startswith("32bit")
            or x.startswith("mongodb-")
            or x.startswith("debian")
            or x.startswith("mongo-cxx-driver")
            or x.startswith("sqlite")
            or "gotools" in x
            or x.find("mozjs") != -1):
            continue

        def isFollowableDir(prefix, full):
            if not os.path.isdir(full):
                return False
            if not os.path.islink(full):
                return True
            # Follow softlinks in the modules directory (e.g: enterprise).
            if os.path.split(prefix)[1] == "modules":
                return True
            return False

        full = prefix + "/" + x
        if isFollowableDir(prefix, full):
            getAllSourceFiles(arr, full)
        else:
            if full.endswith(".cpp") or full.endswith(".h") or full.endswith(".c"):
                full = full.replace("//", "/")
                arr.append(full)

    return arr


def getGitBranch():
    if not os.path.exists(".git") or not os.path.isdir(".git"):
        return None

    version = open(".git/HEAD", "r").read().strip()
    if not version.startswith("ref: "):
        return version
    version = version.split("/")
    version = version[len(version)-1]
    return version


def getGitBranchString(prefix="", postfix=""):
    t = re.compile("[/\\\]").split(os.getcwd())
    if len(t) > 2 and t[len(t)-1] == "mongo":
        par = t[len(t)-2]
        m = re.compile(".*_([vV]\d+\.\d+)$").match(par)
        if m is not None:
            return prefix + m.group(1).lower() + postfix
        if par.find("Nightly") > 0:
            return ""

    b = getGitBranch()
    if b is None or b == "master":
        return ""
    return prefix + b + postfix


def getGitVersion():
    if not os.path.exists(".git") or not os.path.isdir(".git"):
        return "nogitversion"

    version = open(".git/HEAD", "r").read().strip()
    if not version.startswith("ref: "):
        return version
    version = version[5:]
    f = ".git/" + version
    if not os.path.exists(f):
        return version
    return open(f, "r").read().strip()


def getGitDescribe():
    with open(os.devnull, "r+") as devnull:
        proc = subprocess.Popen(
            "git describe",
            stdout=subprocess.PIPE,
            stderr=devnull,
            stdin=devnull,
            shell=True)
        return proc.communicate()[0].strip()


def execsys(args):
    import subprocess
    if isinstance(args, str):
        r = re.compile("\s+")
        args = r.split(args)
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    r = p.communicate()
    return r


def which(executable):
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


def find_python(min_version=(2, 5)):
    try:
        if sys.version_info >= min_version:
            return sys.executable
    except AttributeError:
        # In case the version of Python is somehow missing sys.version_info or sys.executable.
        pass

    version = re.compile(r"[Pp]ython ([\d\.]+)", re.MULTILINE)
    binaries = (
        "python27", "python2.7", "python26", "python2.6", "python25", "python2.5", "python")
    for binary in binaries:
        try:
            out, err = subprocess.Popen(
                [binary, "-V"], stdout=subprocess.PIPE, stderr=subprocess.PIPE).communicate()
            for stream in (out, err):
                match = version.search(stream)
                if match:
                    versiontuple = tuple(map(int, match.group(1).split(".")))
                    if versiontuple >= min_version:
                        return which(binary)
        except:
            pass

    raise Exception(
        "could not find suitable Python (version >= %s)" % ".".join(str(v) for v in min_version))


# unicode is a pain. some strings cannot be unicode()'d
# but we want to just preserve the bytes in a human-readable
# fashion. this codec error handler will substitute the
# repr() of the offending bytes into the decoded string
# at the position they occurred
def replace_with_repr(unicode_error):
    offender = unicode_error.object[unicode_error.start:unicode_error.end]
    return (unicode(repr(offender).strip("'").strip('"')), unicode_error.end)

codecs.register_error("repr", replace_with_repr)
