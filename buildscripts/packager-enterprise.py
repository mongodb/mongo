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
# apt-get install dpkg-dev rpm debhelper fakeroot ia32-libs createrepo git-core libsnmp15
# echo "Now put the dist gnupg signing keys in ~root/.gnupg"

import argparse
import errno
import getopt
from glob import glob
import packager
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urlparse

# The MongoDB names for the architectures we support.
DEFAULT_ARCHES=["x86_64"]

# Made up names for the flavors of distribution we package for.
DISTROS=["suse", "debian","redhat","ubuntu"]


class Spec(object):
    def __init__(self, ver, gitspec = None, rel = None):
        self.ver = ver
        self.gitspec = gitspec
        self.rel = rel

    # Nightly version numbers can be in the form: 3.0.7-pre-, or 3.0.7-5-g3b67ac
    #
    def is_nightly(self):
        return bool(re.search("-$", self.version())) or bool(re.search("\d-\d+-g[0-9a-f]+$", self.version()))

    def is_rc(self):
        return bool(re.search("-rc\d+$", self.version()))

    def is_pre_release(self):
        return self.is_rc() or self.is_nightly()

    def version(self):
        return self.ver

    def metadata_gitspec(self):
        """Git revision to use for spec+control+init+manpage files.
           The default is the release tag for the version being packaged."""
        if(self.gitspec):
          return self.gitspec
        else:
          return 'r' + self.version()

    def version_better_than(self, version_string):
        # FIXME: this is wrong, but I'm in a hurry.
        # e.g., "1.8.2" < "1.8.10", "1.8.2" < "1.8.2-rc1"
        return self.ver > version_string

    def suffix(self):
        return "-enterprise" if int(self.ver.split(".")[1])%2==0 else "-enterprise-unstable"

    def prelease(self):
      # "N" is either passed in on the command line, or "1"
      #
      # 1) Standard release - "N" 
      # 2) Nightly (snapshot) - "0.N.YYYYMMDDlatest"
      # 3) RC's - "0.N.rcX"
      if self.rel:
        corenum = self.rel
      else:
        corenum = 1
      # RC's
      if self.is_rc():
        return "0.%s.%s" % (corenum, re.sub('.*-','',self.version()))
      # Nightlies
      elif self.is_nightly():
        return "0.%s.%s" % (corenum, time.strftime("%Y%m%d"))
      else:
        return str(corenum)


    def pversion(self, distro):
        # Note: Debian packages have funny rules about dashes in
        # version numbers, and RPM simply forbids dashes.  pversion
        # will be the package's version number (but we need to know
        # our upstream version too).
        if re.search("^(debian|ubuntu)", distro.name()):
            return re.sub("-", "~", self.ver)
        elif re.search("(suse|redhat|fedora|centos)", distro.name()):
            return re.sub("-.*", "", self.ver)
        else:
            raise Exception("BUG: unsupported platform?")

    def branch(self):
        """Return the major and minor portions of the specified version.
        For example, if the version is "2.5.5" the branch would be "2.5"
        """
        return ".".join(self.ver.split(".")[0:2])

class Distro(object):
    def __init__(self, string):
        self.n=string

    def name(self):
        return self.n

    def pkgbase(self):
        return "mongodb"

    def archname(self, arch):
        if re.search("^(debian|ubuntu)", self.n):
            return "i386" if arch.endswith("86") else "amd64"
        elif re.search("^(suse|centos|redhat|fedora)", self.n):
            return "i686" if arch.endswith("86") else "x86_64"
        else:
            raise Exception("BUG: unsupported platform?")

    def repodir(self, arch, build_os, spec):
        """Return the directory where we'll place the package files for
        (distro, distro_version) in that distro's preferred repository
        layout (as distinct from where that distro's packaging building
        tools place the package files).

        Packages will go into repos corresponding to the major release 
        series (2.5, 2.6, 2.7, 2.8, etc.) except for RC's and nightlies 
        which will go into special separate "testing" directories 

        Examples:

        repo/apt/ubuntu/dists/precise/mongodb-enterprise/testing/multiverse/binary-amd64
        repo/apt/ubuntu/dists/precise/mongodb-enterprise/testing/multiverse/binary-i386

        repo/apt/ubuntu/dists/precise/mongodb-enterprise/2.5/multiverse/binary-amd64
        repo/apt/ubuntu/dists/precise/mongodb-enterprise/2.5/multiverse/binary-i386

        repo/apt/ubuntu/dists/trusty/mongodb-enterprise/2.5/multiverse/binary-amd64
        repo/apt/ubuntu/dists/trusty/mongodb-enterprise/2.5/multiverse/binary-i386

        repo/apt/debian/dists/wheezy/mongodb-enterprise/2.5/main/binary-amd64
        repo/apt/debian/dists/wheezy/mongodb-enterprise/2.5/main/binary-i386

        repo/yum/redhat/6/mongodb-enterprise/2.5/x86_64
        repo/yum/redhat/6/mongodb-enterprise/2.5/i386

        repo/zypper/suse/11/mongodb-enterprise/2.5/x86_64
        repo/zypper/suse/11/mongodb-enterprise/2.5/i386

        repo/zypper/suse/11/mongodb-enterprise/testing/x86_64
        repo/zypper/suse/11/mongodb-enterprise/testing/i386

        """

        repo_directory = ""

        if spec.is_pre_release():
          repo_directory = "testing"
        else:
          repo_directory = spec.branch()

        if re.search("^(debian|ubuntu)", self.n):
            return "repo/apt/%s/dists/%s/mongodb-enterprise/%s/%s/binary-%s/" % (self.n, self.repo_os_version(build_os), repo_directory, self.repo_component(), self.archname(arch))
        elif re.search("(redhat|fedora|centos)", self.n):
            return "repo/yum/%s/%s/mongodb-enterprise/%s/%s/RPMS/" % (self.n, self.repo_os_version(build_os), repo_directory, self.archname(arch))
        elif re.search("(suse)", self.n):
            return "repo/zypper/%s/%s/mongodb-enterprise/%s/%s/RPMS/" % (self.n, self.repo_os_version(build_os), repo_directory, self.archname(arch))
        else:
            raise Exception("BUG: unsupported platform?")

    def repo_component(self):
        """Return the name of the section/component/pool we are publishing into -
        e.g. "multiverse" for Ubuntu, "main" for debian."""
        if self.n == 'ubuntu':
          return "multiverse"
        elif self.n == 'debian':
          return "main"
        else:
            raise Exception("unsupported distro: %s" % self.n)

    def repo_os_version(self, build_os):
        """Return an OS version suitable for package repo directory
        naming - e.g. 5, 6 or 7 for redhat/centos, "precise," "wheezy," etc.
        for Ubuntu/Debian, 11 for suse"""
        if self.n == 'suse':
            return re.sub(r'^suse(\d+)$', r'\1', build_os)
        if self.n == 'redhat':
            return re.sub(r'^rhel(\d).*$', r'\1', build_os)
        elif self.n == 'ubuntu':
            if build_os == 'ubuntu1204':
                return "precise"
            elif build_os == 'ubuntu1404':
                return "trusty"
            else:
                raise Exception("unsupported build_os: %s" % build_os)
        elif self.n == 'debian':
            if build_os == 'debian71':
                return 'wheezy'
            else:
                raise Exception("unsupported build_os: %s" % build_os)
        else:
            raise Exception("unsupported distro: %s" % self.n)

    def make_pkg(self, build_os, arch, spec, srcdir):
        if re.search("^(debian|ubuntu)", self.n):
            return packager.make_deb(self, build_os, arch, spec, srcdir)
        elif re.search("^(suse|centos|redhat|fedora)", self.n):
            return packager.make_rpm(self, build_os, arch, spec, srcdir)
        else:
            raise Exception("BUG: unsupported platform?")

    def build_os(self):
        """Return the build os label in the binary package to download ("rhel57", "rhel62" and "rhel70"
        for redhat, "ubuntu1204" and "ubuntu1404" for Ubuntu, "debian71" for Debian), and "suse11"
        for SUSE)"""

        if re.search("(suse)", self.n):
            return [ "suse11", "suse12" ]
        if re.search("(redhat|fedora|centos)", self.n):
            return [ "rhel70", "rhel62", "rhel57" ]
        elif self.n == 'ubuntu':
            return [ "ubuntu1204", "ubuntu1404" ]
        elif self.n == 'debian':
            return [ "debian71" ]
        else:
            raise Exception("BUG: unsupported platform?")

    def release_dist(self, build_os):
        """Return the release distribution to use in the rpm - "el5" for rhel 5.x,
        "el6" for rhel 6.x, return anything else unchanged"""

        return re.sub(r'^rh(el\d).*$', r'\1', build_os)

def main(argv):

    distros=[Distro(distro) for distro in DISTROS]

    args = packager.get_args(distros)

    spec = Spec(args.server_version, args.metadata_gitspec, args.release_number)

    oldcwd=os.getcwd()
    srcdir=oldcwd+"/../"

    # Where to do all of our work. Use a randomly-created directory if one
    # is not passed in.
    prefix = args.prefix
    if prefix is None:
      prefix=tempfile.mkdtemp()

    print "Working in directory %s" % prefix

    os.chdir(prefix)
    try:
      # Download the binaries.
      urlfmt="http://downloads.mongodb.com/linux/mongodb-linux-%s-enterprise-%s-%s.tgz"

      # Build a package for each distro/spec/arch tuple, and
      # accumulate the repository-layout directories.
      for (distro, arch) in packager.crossproduct(distros, args.arches):

          for build_os in distro.build_os():
            if build_os in args.distros or not args.distros:

              if args.tarball:
                filename = tarfile(build_os, arch, spec)
                packager.ensure_dir(filename)
                shutil.copyfile(args.tarball,filename)
              else:
                packager.httpget(urlfmt % (arch, build_os, spec.version()), packager.ensure_dir(tarfile(build_os, arch, spec)))

              repo = make_package(distro, build_os, arch, spec, srcdir)
              make_repo(repo, distro, build_os, spec)

    finally:
        os.chdir(oldcwd)

def tarfile(build_os, arch, spec):
    """Return the location where we store the downloaded tarball for
    this package"""
    return "dl/mongodb-linux-%s-enterprise-%s-%s.tar.gz" % (spec.version(), build_os, arch)

def setupdir(distro, build_os, arch, spec):
    # The setupdir will be a directory containing all inputs to the
    # distro's packaging tools (e.g., package metadata files, init
    # scripts, etc), along with the already-built binaries).  In case
    # the following format string is unclear, an example setupdir
    # would be dst/x86_64/debian-sysvinit/wheezy/mongodb-org-unstable/
    # or dst/x86_64/redhat/rhel57/mongodb-org-unstable/
    return "dst/%s/%s/%s/%s%s-%s/" % (arch, distro.name(), build_os, distro.pkgbase(), spec.suffix(), spec.pversion(distro))

def unpack_binaries_into(build_os, arch, spec, where):
    """Unpack the tarfile for (build_os, arch, spec) into directory where."""
    rootdir=os.getcwd()
    packager.ensure_dir(where)
    # Note: POSIX tar doesn't require support for gtar's "-C" option,
    # and Python's tarfile module prior to Python 2.7 doesn't have the
    # features to make this detail easy.  So we'll just do the dumb
    # thing and chdir into where and run tar there.
    os.chdir(where)
    try:
	packager.sysassert(["tar", "xvzf", rootdir+"/"+tarfile(build_os, arch, spec)])
    	release_dir = glob('mongodb-linux-*')[0]
        for releasefile in "bin", "snmp", "LICENSE.txt", "README", "THIRD-PARTY-NOTICES", "MPL-2":
            os.rename("%s/%s" % (release_dir, releasefile), releasefile)
        os.rmdir(release_dir)
    except Exception:
        exc=sys.exc_value
        os.chdir(rootdir)
        raise exc
    os.chdir(rootdir)

def make_package(distro, build_os, arch, spec, srcdir):
    """Construct the package for (arch, distro, spec), getting
    packaging files from srcdir and any user-specified suffix from
    suffixes"""

    sdir=setupdir(distro, build_os, arch, spec)
    packager.ensure_dir(sdir)
    # Note that the RPM packages get their man pages from the debian
    # directory, so the debian directory is needed in all cases (and
    # innocuous in the debianoids' sdirs).
    for pkgdir in ["debian", "rpm"]:
        print "Copying packaging files from %s to %s" % ("%s/%s" % (srcdir, pkgdir), sdir)
        # FIXME: sh-dash-cee is bad. See if tarfile can do this.
        packager.sysassert(["sh", "-c", "(cd \"%s\" && git archive %s %s/ ) | (cd \"%s\" && tar xvf -)" % (srcdir, spec.metadata_gitspec(), pkgdir, sdir)])
    # Splat the binaries and snmp files under sdir.  The "build" stages of the
    # packaging infrastructure will move the files to wherever they
    # need to go.
    unpack_binaries_into(build_os, arch, spec, sdir)
    # Remove the mongosniff binary due to libpcap dynamic
    # linkage.  FIXME: this removal should go away
    # eventually.
    if os.path.exists(sdir + "bin/mongosniff"):
      os.unlink(sdir + "bin/mongosniff")
    return distro.make_pkg(build_os, arch, spec, srcdir)

def make_repo(repodir, distro, build_os, spec):
    if re.search("(debian|ubuntu)", repodir):
        make_deb_repo(repodir, distro, build_os, spec)
    elif re.search("(suse|centos|redhat|fedora)", repodir):
        packager.make_rpm_repo(repodir)
    else:
        raise Exception("BUG: unsupported platform?")

def make_deb_repo(repo, distro, build_os, spec):
    # Note: the Debian repository Packages files must be generated
    # very carefully in order to be usable.
    oldpwd=os.getcwd()
    os.chdir(repo+"../../../../../../")
    try:
        dirs=set([os.path.dirname(deb)[2:] for deb in packager.backtick(["find", ".", "-name", "*.deb"]).split()])
        for d in dirs:
            s=packager.backtick(["dpkg-scanpackages", d, "/dev/null"])
            with open(d+"/Packages", "w") as f:
                f.write(s)
            b=packager.backtick(["gzip", "-9c", d+"/Packages"])
            with open(d+"/Packages.gz", "wb") as f:
                f.write(b)
    finally:
        os.chdir(oldpwd)
    # Notes: the Release{,.gpg} files must live in a special place,
    # and must be created after all the Packages.gz files have been
    # done.
    s="""Origin: mongodb
Label: mongodb
Suite: %s
Codename: %s/mongodb-enterprise
Architectures: amd64
Components: %s
Description: MongoDB packages
""" % (distro.repo_os_version(build_os), distro.repo_os_version(build_os), distro.repo_component())
    if os.path.exists(repo+"../../Release"):
        os.unlink(repo+"../../Release")
    if os.path.exists(repo+"../../Release.gpg"):
        os.unlink(repo+"../../Release.gpg")
    oldpwd=os.getcwd()
    os.chdir(repo+"../../")
    s2=packager.backtick(["apt-ftparchive", "release", "."])
    try:
        with open("Release", 'w') as f:
            f.write(s)
            f.write(s2)
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
        packager.sysassert(["cp", "-rv", src + "/" + r, dname])

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

if __name__ == "__main__":
    main(sys.argv)
