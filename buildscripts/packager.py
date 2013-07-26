#!/usr/bin/python

# This program makes Debian and RPM repositories for MongoDB, by
# downloading our tarballs of statically linked executables and
# insinuating them into Linux packages.  It must be run on a
# Debianoid, since Debian provides tools to make RPMs, but RPM-based
# systems don't provide debian packaging crud.

# Notes:
#
# * Almost anything that you want to be able to influence about how a
# package construction must be embedded in some file that the
# packaging tool uses for input (e.g., debian/rules, debian/control,
# debian/changelog; or the RPM specfile), and the precise details are
# arbitrary and silly.  So this program generates all the relevant
# inputs to the packaging tools.
#
# * Once a .deb or .rpm package is made, there's a separate layer of
# tools that makes a "repository" for use by the apt/yum layers of
# package tools.  The layouts of these repositories are arbitrary and
# silly, too.
#
# * Before you run the program on a new host, these are the
# prerequisites:
#
# apt-get install dpkg-dev rpm debhelper fakeroot ia32-libs createrepo git-core
# echo "Now put the dist gnupg signing keys in ~root/.gnupg"

import errno
import getopt
import httplib2
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
import urlparse

# For the moment, this program runs on the host that also serves our
# repositories to the world, so the last thing the program does is
# move the repositories into place.  Make this be the path where the
# web server will look for repositories.
REPOPATH="/var/www/repo"

# The 10gen names for the architectures we support.
ARCHES=["i686", "x86_64"]

# Made up names for the flavors of distribution we package for.
DISTROS=["debian-sysvinit", "ubuntu-upstart", "redhat"]

# When we're preparing a directory containing packaging tool inputs
# and our binaries, use this relative subdirectory for placing the
# binaries.
BINARYDIR="BINARIES"

class Spec(object):
    def __init__(self, specstr):
        tup = specstr.split(":")
        self.ver = tup[0]
        # Hack: the second item in the tuple is treated as a suffix if
        # it lacks an equals sign; otherwise it's the start of named
        # parameters.
        self.suf = None
        if len(tup) > 1 and tup[1].find("=") == -1:
            self.suf = tup[1]
        # Catch-all for any other parameters to the packaging.
        i = 2 if self.suf else 1
        self.params = dict([s.split("=") for s in tup[i:]])
        for key in self.params.keys():
            assert(key in ["suffix", "revision"])

    def version(self):
        return self.ver

    def version_better_than(self, version_string):
        # FIXME: this is wrong, but I'm in a hurry.
        # e.g., "1.8.2" < "1.8.10", "1.8.2" < "1.8.2-rc1"
        return self.ver > version_string

    def suffix(self):
        # suffix is what we tack on after pkgbase.
        if self.suf:
            return self.suf
        elif "suffix" in self.params:
            return self.params["suffix"]
        else:
            return "-10gen" if int(self.ver.split(".")[1])%2==0 else "-10gen-unstable"


    def pversion(self, distro):
        # Note: Debian packages have funny rules about dashes in
        # version numbers, and RPM simply forbids dashes.  pversion
        # will be the package's version number (but we need to know
        # our upstream version too).
        if re.search("^(debian|ubuntu)", distro.name()):
            return re.sub("-", "~", self.ver)                
        elif re.search("(redhat|fedora|centos)", distro.name()):
            return re.sub("\\d+-", "", self.ver)
        else:
            raise Exception("BUG: unsupported platform?")

    def param(self, param):
        if param in self.params:
            return self.params[param]
        return None

class Distro(object):
    def __init__(self, string):
        self.n=string

    def name(self):
        return self.n

    def pkgbase(self):
        # pkgbase is the first part of the package's name on
        # this distro.
        return "mongo" if re.search("(redhat|fedora|centos)", self.n) else "mongodb"

    def archname(self, arch):
        if re.search("^(debian|ubuntu)", self.n):
            return "i386" if arch.endswith("86") else "amd64"
        elif re.search("^(centos|redhat|fedora)", self.n):
            return "i686" if arch.endswith("86") else "x86_64"
        else:
            raise Exception("BUG: unsupported platform?")

    def repodir(self, arch):
        """Return the directory where we'll place the package files for
        (distro, distro_version) in that distro's preferred repository
        layout (as distinct from where that distro's packaging building
        tools place the package files)."""
        if re.search("^(debian|ubuntu)", self.n):
            return "repo/%s/dists/dist/10gen/binary-%s/" % (self.n, self.archname(arch))
        elif re.search("(redhat|fedora|centos)", self.n):
            return "repo/%s/os/%s/RPMS/" % (self.n, self.archname(arch))
        else:
            raise Exception("BUG: unsupported platform?")
    
    def make_pkg(self, arch, spec, srcdir):
        if re.search("^(debian|ubuntu)", self.n):
            return make_deb(self, arch, spec, srcdir)
        elif re.search("^(centos|redhat|fedora)", self.n):
            return make_rpm(self, arch, spec, srcdir)
        else:
            raise Exception("BUG: unsupported platform?")

def main(argv):
    (flags, specs) = parse_args(argv[1:])
    distros=[Distro(distro) for distro in DISTROS]

    oldcwd=os.getcwd()
    srcdir=oldcwd+"/../"

    # We do all our work in a randomly-created directory. You can set
    # TEMPDIR to influence where this program will do stuff.
    prefix=tempfile.mkdtemp()
    print "Working in directory %s" % prefix

    # This will be a list of directories where we put packages in
    # "repository layout".
    repos=[]

    os.chdir(prefix)
    try:
        # Download the binaries.
        urlfmt="http://fastdl.mongodb.org/linux/mongodb-linux-%s-%s.tgz"
        for (spec, arch) in crossproduct(specs, ARCHES):
            httpget(urlfmt % (arch, spec.version()), ensure_dir(tarfile(arch, spec)))
    
        # Build a pacakge for each distro/spec/arch tuple, and
        # accumulate the repository-layout directories.
        for (distro, spec, arch) in crossproduct(distros, specs, ARCHES):
            repos.append(make_package(distro, arch, spec, srcdir))
    
        # Build the repos' metadatas.
        for repo in set(repos):
            print repo
            make_repo(repo)
    
    finally:
        os.chdir(oldcwd)
    if "-n" not in flags:
        move_repos_into_place(prefix+"/repo", REPOPATH)
        # FIXME: try shutil.rmtree some day.
        sysassert(["rm", "-rv", prefix])

def parse_args(args):
    if len(args) == 0:
        print """Usage: packager.py [OPTS] SPEC1 SPEC2 ... SPECn

Options:

  -n:  Just build the packages, don't publish them as a repo 
       or clean out the working directory

Each SPEC is a mongodb version string optionally followed by a colon
and some parameters, of the form <paramname>=<value>.  Supported
parameters:

  suffix -- suffix to append to the package's base name.  (If
            unsupplied, suffixes default based on the parity of the
            middle number in the version.)

  revision -- least-order version number to packaging systems
"""
        sys.exit(0)

    try:
        (flags, args) = getopt.getopt(args, "n")
    except getopt.GetoptError, err:
        print str(err)
        sys.exit(2)
    flags=dict(flags)
    specs=[Spec(arg) for arg in args]
    return (flags, specs)

def crossproduct(*seqs):
    """A generator for iterating all the tuples consisting of elements
    of seqs."""
    l = len(seqs)
    if l == 0:
        pass
    elif l == 1:
        for i in seqs[0]:
            yield [i]
    else:
        for lst in crossproduct(*seqs[:-1]):
            for i in seqs[-1]:
                lst2=list(lst)
                lst2.append(i)
                yield lst2

def sysassert(argv):
    """Run argv and assert that it exited with status 0."""
    print "In %s, running %s" % (os.getcwd(), " ".join(argv))
    sys.stdout.flush()
    sys.stderr.flush()
    assert(subprocess.Popen(argv).wait()==0)

def backtick(argv):
    """Run argv and return its output string."""
    print "In %s, running %s" % (os.getcwd(), " ".join(argv))
    sys.stdout.flush()
    sys.stderr.flush()
    return subprocess.Popen(argv, stdout=subprocess.PIPE).communicate()[0]

def ensure_dir(filename):
    """Make sure that the directory that's the dirname part of
    filename exists, and return filename."""
    dirpart = os.path.dirname(filename)
    try:
        os.makedirs(dirpart)
    except OSError: # as exc: # Python >2.5
        exc=sys.exc_value
        if exc.errno == errno.EEXIST:
            pass
        else: 
            raise exc
    return filename


def tarfile(arch, spec):
    """Return the location where we store the downloaded tarball for
    (arch, spec)"""
    return "dl/mongodb-linux-%s-%s.tar.gz" % (spec.version(), arch)

def setupdir(distro, arch, spec):
    # The setupdir will be a directory containing all inputs to the
    # distro's packaging tools (e.g., package metadata files, init
    # scripts, etc), along with the already-built binaries).  In case
    # the following format string is unclear, an example setupdir
    # would be dst/x86_64/debian-sysvinit/mongodb-10gen-unstable/
    return "dst/%s/%s/%s%s-%s/" % (arch, distro.name(), distro.pkgbase(), spec.suffix(), spec.pversion(distro))

def httpget(url, filename):
    """Download the contents of url to filename, return filename."""
    print "Fetching %s to %s." % (url, filename)
    conn = None
    u=urlparse.urlparse(url)
    assert(u.scheme=='http')
    try:
        h = httplib2.Http(cache = os.environ["HOME"] + "/.cache")
        resp, content = h.request(url, "GET")
        t=filename+'.TMP'
        if resp.status==200:
            f = open(t, 'w')
            try:
                f.write(content)
            finally:
                f.close()
        else:
            raise Exception("HTTP error %d" % resp.status)
        os.rename(t, filename)
    finally:
        if conn:
            conn.close()
    return filename

def unpack_binaries_into(arch, spec, where):
    """Unpack the tarfile for (arch, spec) into directory where."""
    rootdir=os.getcwd()
    ensure_dir(where)
    # Note: POSIX tar doesn't require support for gtar's "-C" option,
    # and Python's tarfile module prior to Python 2.7 doesn't have the
    # features to make this detail easy.  So we'll just do the dumb
    # thing and chdir into where and run tar there.
    os.chdir(where)
    try:
        sysassert(["tar", "xvzf", rootdir+"/"+tarfile(arch, spec), "mongodb-linux-%s-%s/bin" % (arch, spec.version())])
        os.rename("mongodb-linux-%s-%s/bin" % (arch, spec.version()), "bin")
        os.rmdir("mongodb-linux-%s-%s" % (arch, spec.version()))
    except Exception:
        exc=sys.exc_value
        os.chdir(rootdir)
        raise exc
    os.chdir(rootdir)

def make_package(distro, arch, spec, srcdir):
    """Construct the package for (arch, distro, spec), getting
    packaging files from srcdir and any user-specified suffix from
    suffixes"""

    sdir=setupdir(distro, arch, spec)
    ensure_dir(sdir)
    # Note that the RPM packages get their man pages from the debian
    # directory, so the debian directory is needed in all cases (and
    # innocuous in the debianoids' sdirs).
    for pkgdir in ["debian", "rpm"]:
        print "Copying packaging files from %s to %s" % ("%s/%s" % (srcdir, pkgdir), sdir)
        # FIXME: sh-dash-cee is bad. See if tarfile can do this.
        sysassert(["sh", "-c", "(cd \"%s\" && git archive r%s %s/ ) | (cd \"%s\" && tar xvf -)" % (srcdir, spec.version(), pkgdir, sdir)])
    # Splat the binaries under sdir.  The "build" stages of the
    # packaging infrastructure will move the binaries to wherever they
    # need to go.  
    unpack_binaries_into(arch, spec, sdir+("%s/usr/"%BINARYDIR))
    # Remove the mongosniff binary due to libpcap dynamic
    # linkage.  FIXME: this removal should go away
    # eventually.
    os.unlink(sdir+("%s/usr/bin/mongosniff"%BINARYDIR))
    return distro.make_pkg(arch, spec, srcdir)

def make_repo(repodir):
    if re.search("(debian|ubuntu)", repodir):
        make_deb_repo(repodir)
    elif re.search("(centos|redhat|fedora)", repodir):
        make_rpm_repo(repodir)
    else:
        raise Exception("BUG: unsupported platform?")

def make_deb(distro, arch, spec, srcdir):
    # I can't remember the details anymore, but the initscript/upstart
    # job files' names must match the package name in some way; and
    # see also the --name flag to dh_installinit in the generated
    # debian/rules file.
    suffix=spec.suffix()
    sdir=setupdir(distro, arch, spec)
    if re.search("sysvinit", distro.name()):
        os.link(sdir+"debian/init.d", sdir+"debian/%s%s.mongodb.init" % (distro.pkgbase(), suffix))
        os.unlink(sdir+"debian/mongodb.upstart")
    elif re.search("upstart", distro.name()):
        os.link(sdir+"debian/mongodb.upstart", sdir+"debian/%s%s.upstart" % (distro.pkgbase(), suffix))
        os.unlink(sdir+"debian/init.d")
    else:
        raise Exception("unknown debianoid flavor: not sysvinit or upstart?")
    write_debian_changelog(sdir+"debian/changelog", spec, srcdir)
    distro_arch=distro.archname(arch)
    sysassert(["cp", "-v", srcdir+"debian/control", sdir+"debian/"])
    sysassert(["cp", "-v", srcdir+"debian/rules", sdir+"debian/"])
    sysassert(["cp", "-v", srcdir+"debian/rules", sdir+"debian/"])
    # old non-server-package postinst will be hanging around for old versions
    #
    os.unlink(sdir+"debian/postinst")

    # copy our postinst files
    #
    sysassert(["sh", "-c", "cp -v \"%sdebian/\"*.postinst \"%sdebian/\""%(srcdir, sdir)])

    # Do the packaging.
    oldcwd=os.getcwd()
    try:
        os.chdir(sdir)
        sysassert(["dpkg-buildpackage", "-a"+distro_arch, "-k Richard Kreuter <richard@10gen.com>"])
    finally:
        os.chdir(oldcwd)
    r=distro.repodir(arch)
    ensure_dir(r)
    # FIXME: see if shutil.copyfile or something can do this without
    # much pain.
    # sysassert(["cp", "-v", sdir+"../%s%s_%s%s_%s.deb"%(distro.pkgbase(), suffix, spec.pversion(distro), "-"+spec.param("revision") if spec.param("revision") else"", distro_arch), r])
    # sysassert(["cp", "-v", sdir+"../*.deb", r])
    sysassert(["sh", "-c", "cp -v \"%s/../\"*.deb \"%s\""%(sdir, r)])
    return r

def make_deb_repo(repo):
    # Note: the Debian repository Packages files must be generated
    # very carefully in order to be usable.
    oldpwd=os.getcwd()
    os.chdir(repo+"../../../../")
    try:
        dirs=set([os.path.dirname(deb)[2:] for deb in backtick(["find", ".", "-name", "*.deb"]).split()])
        for d in dirs:
            s=backtick(["dpkg-scanpackages", d, "/dev/null"])
            f=open(d+"/Packages", "w")
            try:
                f.write(s)
            finally:
                f.close()
            b=backtick(["gzip", "-9c", d+"/Packages"])
            f=open(d+"/Packages.gz", "wb")
            try:
                f.write(b)
            finally:
                f.close()
    finally:
        os.chdir(oldpwd)
    # Notes: the Release{,.gpg} files must live in a special place,
    # and must be created after all the Packages.gz files have been
    # done.
    s="""
Origin: 10gen
Label: 10gen
Suite: 10gen
Codename: %s
Version: %s
Architectures: i386 amd64
Components: 10gen
Description: 10gen packages
""" % ("dist", "dist")
    if os.path.exists(repo+"../../Release"):
        os.unlink(repo+"../../Release")
    if os.path.exists(repo+"../../Release.gpg"):
        os.unlink(repo+"../../Release.gpg")
    oldpwd=os.getcwd()
    os.chdir(repo+"../../")
    s2=backtick(["apt-ftparchive", "release", "."])
    try:
        f=open("Release", 'w')
        try:
            f.write(s)
            f.write(s2)
        finally:
            f.close()

        arg=None
        for line in backtick(["gpg", "--list-keys"]).split("\n"):
            tokens=line.split()
            if len(tokens)>0 and tokens[0] == "uid":
                arg=tokens[-1]
                break
        # Note: for some reason, I think --no-tty might be needed
        # here, but maybe not.
        sysassert(["gpg", "-r", arg, "--no-secmem-warning", "-abs", "--output", "Release.gpg", "Release"])
    finally:
        os.chdir(oldpwd)


def move_repos_into_place(src, dst):
    # Find all the stuff in src/*, move it to a freshly-created
    # directory beside dst, then play some games with symlinks so that
    # dst is a name the new stuff and dst+".old" names the previous
    # one.  This feels like a lot of hooey for something so trivial.

    # First, make a crispy fresh new directory to put the stuff in.
    i=0
    while True:
        date_suffix=time.strftime("%Y-%m-%d")
        dname=dst+".%s.%d" % (date_suffix, i)
        try:
            os.mkdir(dname)
            break
        except OSError:
            exc=sys.exc_value
            if exc.errno == errno.EEXIST:
                pass
            else:
                raise exc
        i=i+1

    # Put the stuff in our new directory.
    for r in os.listdir(src):
        sysassert(["cp", "-rv", src + "/" + r, dname])

    # Make a symlink to the new directory; the symlink will be renamed
    # to dst shortly.
    i=0
    while True:
        tmpnam=dst+".TMP.%d" % i
        try:
            os.symlink(dname, tmpnam)
            break
        except OSError: # as exc: # Python >2.5
            exc=sys.exc_value
            if exc.errno == errno.EEXIST:
                pass
            else: 
                raise exc
        i=i+1

    # Make a symlink to the old directory; this symlink will be
    # renamed shortly, too.
    oldnam=None
    if os.path.exists(dst):
       i=0
       while True:
           oldnam=dst+".old.%d" % i
           try:
               os.symlink(os.readlink(dst), oldnam)
               break
           except OSError: # as exc: # Python >2.5
               exc=sys.exc_value
               if exc.errno == errno.EEXIST:
                   pass
               else: 
                   raise exc

    os.rename(tmpnam, dst)
    if oldnam:
        os.rename(oldnam, dst+".old")


def write_debian_changelog(path, spec, srcdir):
    oldcwd=os.getcwd()
    os.chdir(srcdir)
    preamble=""
    if spec.param("revision"):
        preamble="""mongodb%s (%s-%s) unstable; urgency=low

  * Bump revision number

 -- Richard Kreuter <richard@10gen.com>  %s

""" % (spec.suffix(), spec.pversion(Distro("debian")), spec.param("revision"), time.strftime("%a, %d %b %Y %H:%m:%S %z"))
    try:
        s=preamble+backtick(["sh", "-c", "git archive r%s debian/changelog | tar xOf -" % spec.version()])
    finally:
        os.chdir(oldcwd)
    f=open(path, 'w')
    lines=s.split("\n")
    # If the first line starts with "mongodb", it's not a revision
    # preamble, and so frob the version number.
    lines[0]=re.sub("^mongodb \\(.*\\)", "mongodb (%s)" % (spec.pversion(Distro("debian"))), lines[0])
    # Rewrite every changelog entry starting in mongodb<space>
    lines=[re.sub("^mongodb ", "mongodb%s " % (spec.suffix()), l) for l in lines]
    lines=[re.sub("^  --", " --", l) for l in lines]
    s="\n".join(lines)
    try:
        f.write(s)
    finally:
        f.close()
def make_rpm(distro, arch, spec, srcdir):
    # Create the specfile.
    suffix=spec.suffix()
    sdir=setupdir(distro, arch, spec)
    specfile=srcdir+"rpm/mongo%s.spec" % suffix
    topdir=ensure_dir(os.getcwd()+'/rpmbuild/')
    for subdir in ["BUILD", "RPMS", "SOURCES", "SPECS", "SRPMS"]:
        ensure_dir("%s/%s/" % (topdir, subdir))
    distro_arch=distro.archname(arch)
    # RPM tools take these macro files that define variables in
    # RPMland.  Unfortunately, there's no way to tell RPM tools to use
    # a given file *in addition* to the files that it would already
    # load, so we have to figure out what it would normally load,
    # augment that list, and tell RPM to use the augmented list.  To
    # figure out what macrofiles ordinarily get loaded, older RPM
    # versions had a parameter called "macrofiles" that could be
    # extracted from "rpm --showrc".  But newer RPM versions don't
    # have this.  To tell RPM what macros to use, older versions of
    # RPM have a --macros option that doesn't work; on these versions,
    # you can put a "macrofiles" parameter into an rpmrc file.  But
    # that "macrofiles" setting doesn't do anything for newer RPM
    # versions, where you have to use the --macros flag instead.  And
    # all of this is to let us do our work with some guarantee that
    # we're not clobbering anything that doesn't belong to us.  Why is
    # RPM so braindamaged?
    macrofiles=[l for l in backtick(["rpm", "--showrc"]).split("\n") if l.startswith("macrofiles")]
    flags=[]
    macropath=os.getcwd()+"/macros"
    write_rpm_macros_file(macropath, topdir)
    if len(macrofiles)>0:
        macrofiles=macrofiles[0]+":"+macropath
        rcfile=os.getcwd()+"/rpmrc"
        write_rpmrc_file(rcfile, macrofiles)
        flags=["--rpmrc", rcfile]
    else:
        # This hard-coded hooey came from some box running RPM
        # 4.4.2.3.  It may not work over time, but RPM isn't sanely
        # configurable.
        flags=["--macros", "/usr/lib/rpm/macros:/usr/lib/rpm/%s-linux/macros:/etc/rpm/macros.*:/etc/rpm/macros:/etc/rpm/%s-linux/macros:~/.rpmmacros:%s" % (distro_arch, distro_arch, macropath)]
    # Put the specfile and the tar'd up binaries and stuff in
    # place. FIXME: see if shutil.copyfile can do this without too
    # much hassle.
    sysassert(["cp", "-v", specfile, topdir+"SPECS/"])
    oldcwd=os.getcwd()
    os.chdir(sdir+"/../")
    try:
        sysassert(["tar", "-cpzf", topdir+"SOURCES/mongo%s-%s.tar.gz" % (suffix, spec.pversion(distro)), os.path.basename(os.path.dirname(sdir))])
    finally:
        os.chdir(oldcwd)
    # Do the build.
    sysassert(["rpmbuild", "-ba", "--target", distro_arch] + flags + ["%s/SPECS/mongo%s.spec" % (topdir, suffix)])
    r=distro.repodir(arch)
    ensure_dir(r)
    # FIXME: see if some combination of shutil.copy<hoohah> and glob
    # can do this without shelling out.
    sysassert(["sh", "-c", "cp -v \"%s/RPMS/%s/\"*.rpm \"%s\""%(topdir, distro_arch, r)])
    return r

def make_rpm_repo(repo):
    oldpwd=os.getcwd()
    os.chdir(repo+"../")
    try:
        sysassert(["createrepo", "."])
    finally:
        os.chdir(oldpwd)


def write_rpmrc_file(path, string):
    f=open(path, 'w')
    try:
        f.write(string)
    finally:
        f.close()

def write_rpm_macros_file(path, topdir):
    f=open(path, 'w')
    try:
        f.write("%%_topdir	%s" % topdir)
    finally:
        f.close()

if __name__ == "__main__":
    main(sys.argv)
