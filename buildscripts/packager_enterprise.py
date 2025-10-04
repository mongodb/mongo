#!/usr/bin/env python3
"""Packager Enterprise module."""

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
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from glob import glob

import git

sys.path.append(os.getcwd())

import packager

# The MongoDB names for the architectures we support.
ARCH_CHOICES = ["x86_64", "ppc64le", "s390x", "arm64", "aarch64"]

# Made up names for the flavors of distribution we package for.
DISTROS = ["suse", "debian", "redhat", "ubuntu", "amazon", "amazon2", "amazon2023"]


class EnterpriseSpec(packager.Spec):
    """EnterpriseSpec class."""

    def suffix(self):
        return packager.get_suffix(self.ver, "-enterprise", "-enterprise-unstable")
        """Suffix."""

    def move_required_contents(self):
        """Move the required contents to the current working directory.

        Below in the for loop it's the required list of files that needs
        to be moved from the extracted tarball to the current working
        directory. The root path of the tarball content starts with
        mongodb-linux-<suffix> so we glob since the suffix of the root path
        is not a fixed text.
        """
        release_dir = glob("mongodb-linux-*")[0]
        for release_file in (
            "bin",
            "LICENSE-Enterprise.txt",
            "README",
            "THIRD-PARTY-NOTICES",
            "MPL-2",
        ):
            os.rename("%s/%s" % (release_dir, release_file), release_file)
        os.rmdir(release_dir)


class EnterpriseCryptSpec(EnterpriseSpec):
    """EnterpriseCryptSpec class."""

    def suffix(self):
        """Suffix."""
        if int(self.ver.split(".")[1]) == 0:
            return "-enterprise-crypt-v1"
        return "-enterprise-unstable-crypt-v1"

    def move_required_contents(self):
        """Move the required contents to the current working directory

        The files that were extracted from the tarball file are already
        in the current working directory. It does not have a mongodb-linux-...
        as the root content of the tarball file is flat.
        """
        pass


class EnterpriseDistro(packager.Distro):
    """EnterpriseDistro class."""

    def repodir(self, arch, build_os, spec):  # noqa: D406,D407,D412,D413
        """Return the directory where we'll place the package files.

        This is for (distro, distro_version) in that distro's preferred repository
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

        if re.search("^(debian|ubuntu)", self.dname):
            return "repo/apt/%s/dists/%s/mongodb-enterprise/%s/%s/binary-%s/" % (
                self.dname,
                self.repo_os_version(build_os),
                repo_directory,
                self.repo_component(),
                self.archname(arch),
            )
        elif re.search("(redhat|fedora|centos|amazon)", self.dname):
            return "repo/yum/%s/%s/mongodb-enterprise/%s/%s/RPMS/" % (
                self.dname,
                self.repo_os_version(build_os),
                repo_directory,
                self.archname(arch),
            )
        elif re.search("(suse)", self.dname):
            return "repo/zypper/%s/%s/mongodb-enterprise/%s/%s/RPMS/" % (
                self.dname,
                self.repo_os_version(build_os),
                repo_directory,
                self.archname(arch),
            )
        else:
            raise Exception("BUG: unsupported platform?")

    def build_os(self, arch):
        """Return the build os label in the binary package to download.

        The labels "rhel57", "rhel62", "rhel67", "rhel70", "rhel79", "rhel80", "rhel90" are for redhat,
        the others are delegated to the super class.
        """
        if arch == "ppc64le":
            if self.dname == "ubuntu":
                return ["ubuntu1604", "ubuntu1804"]
            if self.dname == "redhat":
                return ["rhel71", "rhel81", "rhel9"]
            return []
        if arch == "s390x":
            if self.dname == "redhat":
                return ["rhel67", "rhel72", "rhel83", "rhel9"]
            if self.dname == "suse":
                return ["suse11", "suse12", "suse15"]
            if self.dname == "ubuntu":
                return ["ubuntu1604", "ubuntu1804"]
            return []
        if arch == "arm64":
            if self.dname == "ubuntu":
                return ["ubuntu1804", "ubuntu2004", "ubuntu2204", "ubuntu2404"]
        if arch == "aarch64":
            if self.dname == "redhat":
                return ["rhel82", "rhel88", "rhel90", "rhel93"]
            if self.dname == "amazon2":
                return ["amazon2"]
            if self.dname == "amazon2023":
                return ["amazon2023"]
            return []

        if re.search("(redhat|fedora|centos)", self.dname):
            return ["rhel90", "rhel93", "rhel80", "rhel70", "rhel79", "rhel62", "rhel57", "rhel88"]
        return super(EnterpriseDistro, self).build_os(arch)


def verify_args(args):
    # If the crypt spec is specified, the tarball file must include the crypt library.
    if args.crypt_spec:
        if (
            "lib/mongo_crypt_v1.so"
            not in subprocess.run(
                [
                    "tar",
                    "-tzf",
                    args.tarball,
                ],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.split()
        ):
            raise ValueError(
                "The tarball file %s does not contain the crypt library. Please use a tarball that contains the crypt library."
                % args.tarball
            )


def main():
    """Execute Main program."""

    distros = [EnterpriseDistro(distro) for distro in DISTROS]

    args = packager.get_args(distros, ARCH_CHOICES)
    verify_args(args)

    spec = get_enterprise_spec(args)

    oldcwd = os.getcwd()
    srcdir = oldcwd + "/../"

    # Where to do all of our work. Use a randomly-created directory if one
    # is not passed in.
    prefix = args.prefix
    if prefix is None:
        prefix = tempfile.mkdtemp()

    print("Working in directory %s" % prefix)

    os.chdir(prefix)
    try:
        made_pkg = False
        # Build a package for each distro/spec/arch tuple, and
        # accumulate the repository-layout directories.
        for distro, arch in packager.crossproduct(distros, args.arches):
            for build_os in distro.build_os(arch):
                if build_os in args.distros or not args.distros:
                    filename = tarfile(build_os, arch, spec)
                    packager.ensure_dir(filename)
                    shutil.copyfile(args.tarball, filename)

                    repo = make_package(distro, build_os, arch, spec, srcdir)
                    make_repo(repo, distro, build_os)

                    made_pkg = True

        if not made_pkg:
            raise Exception("No valid combination of distro and arch selected")

    finally:
        os.chdir(oldcwd)


def get_enterprise_spec(args):
    """Get the EnterpriseSpec."""
    if args.crypt_spec:
        return EnterpriseCryptSpec(args.server_version, args.metadata_gitspec, args.release_number)
    return EnterpriseSpec(args.server_version, args.metadata_gitspec, args.release_number)


def tarfile(build_os, arch, spec):
    """Return the location where we store the downloaded tarball for this package."""
    return "dl/mongodb-linux-%s-enterprise-%s-%s.tar.gz" % (spec.version(), build_os, arch)


def setupdir(distro, build_os, arch, spec):
    """Return the setup directory name."""
    # The setupdir will be a directory containing all inputs to the
    # distro's packaging tools (e.g., package metadata files, init
    # scripts, etc, along with the already-built binaries).  In case
    # the following format string is unclear, an example setupdir
    # would be dst/x86_64/debian-sysvinit/wheezy/mongodb-org-unstable/
    # or dst/x86_64/redhat/rhel57/mongodb-org-unstable/
    return "dst/%s/%s/%s/%s%s-%s/" % (
        arch,
        distro.name(),
        build_os,
        distro.pkgbase(),
        spec.suffix(),
        spec.pversion(distro),
    )


def unpack_binaries_into(build_os, arch, spec, where):
    """Unpack the tarfile for (build_os, arch, spec) into directory where."""
    root_dir = os.getcwd()
    packager.ensure_dir(where)
    # Note: POSIX tar doesn't require support for gtar's "-C" option,
    # and Python's tarfile module prior to Python 2.7 doesn't have the
    # features to make this detail easy.  So we'll just do the dumb
    # thing and chdir into where and run tar there.
    os.chdir(where)
    try:
        packager.sysassert(["tar", "xvzf", root_dir + "/" + tarfile(build_os, arch, spec)])
        spec.move_required_contents()
    except Exception:
        exc = sys.exc_info()[1]
        os.chdir(root_dir)
        raise exc
    os.chdir(root_dir)


def make_package(distro, build_os, arch, spec, srcdir):
    """Construct the package for (arch, distro, spec).

    Get the packaging files from srcdir and any user-specified suffix from suffixes.
    """

    sdir = setupdir(distro, build_os, arch, spec)
    packager.ensure_dir(sdir)
    # Note that the RPM packages get their man pages from the debian
    # directory, so the debian directory is needed in all cases (and
    # innocuous in the debianoids' sdirs).
    for pkgdir in ["debian", "rpm"]:
        print("Copying packaging files from %s to %s" % ("%s/%s" % (srcdir, pkgdir), sdir))
        git_repo = git.Repo(srcdir)
        # get the original HEAD position of repo
        head_commit_sha = git_repo.head.object.hexsha

        # add and commit the uncommited changes
        print("Commiting uncommited changes")
        git_repo.git.add(all=True)
        # only commit changes if there are any
        if len(git_repo.index.diff("HEAD")) != 0:
            with git_repo.git.custom_environment(
                GIT_COMMITTER_NAME="Evergreen", GIT_COMMITTER_EMAIL="evergreen@mongodb.com"
            ):
                git_repo.git.commit("--author='Evergreen <>'", "-m", "temp commit")

        # original command to preserve functionality
        # FIXME: make consistent with the rest of the code when we have more packaging testing
        # FIXME: sh-dash-cee is bad. See if tarfile can do this.
        print("Copying packaging files from specified gitspec:", spec.metadata_gitspec())
        packager.sysassert(
            [
                "sh",
                "-c",
                '(cd "%s" && git archive %s %s/ ) | (cd "%s" && tar xvf -)'
                % (srcdir, spec.metadata_gitspec(), pkgdir, sdir),
            ]
        )

        # reset branch to original state
        print("Resetting branch to original state")
        git_repo.git.reset("--mixed", head_commit_sha)

    # Splat the binaries under sdir.  The "build" stages of the
    # packaging infrastructure will move the files to wherever they
    # need to go.
    unpack_binaries_into(build_os, arch, spec, sdir)

    return distro.make_pkg(build_os, arch, spec, srcdir)


def make_repo(repodir, distro, build_os):
    """Make the repo."""
    if re.search("(debian|ubuntu)", repodir):
        make_deb_repo(repodir, distro, build_os)
    elif re.search("(suse|centos|redhat|fedora|amazon)", repodir):
        packager.make_rpm_repo(repodir)
    else:
        raise Exception("BUG: unsupported platform?")


def make_deb_repo(repo, distro, build_os):
    """Make the Debian repo."""
    # Note: the Debian repository Packages files must be generated
    # very carefully in order to be usable.
    oldpwd = os.getcwd()
    os.chdir(repo + "../../../../../../")
    try:
        dirs = {
            os.path.dirname(deb)[2:]
            for deb in packager.backtick(["find", ".", "-name", "*.deb"]).decode("utf-8").split()
        }
        for directory in dirs:
            st = packager.backtick(["dpkg-scanpackages", directory, "/dev/null"])
            with open(directory + "/Packages", "wb") as fh:
                fh.write(st)
            bt = packager.backtick(["gzip", "-9c", directory + "/Packages"])
            with open(directory + "/Packages.gz", "wb") as fh:
                fh.write(bt)
    finally:
        os.chdir(oldpwd)
    # Notes: the Release{,.gpg} files must live in a special place,
    # and must be created after all the Packages.gz files have been
    # done.
    s1 = """Origin: mongodb
Label: mongodb
Suite: %s
Codename: %s/mongodb-enterprise
Architectures: amd64 ppc64el s390x arm64
Components: %s
Description: MongoDB packages
""" % (distro.repo_os_version(build_os), distro.repo_os_version(build_os), distro.repo_component())
    if os.path.exists(repo + "../../Release"):
        os.unlink(repo + "../../Release")
    if os.path.exists(repo + "../../Release.gpg"):
        os.unlink(repo + "../../Release.gpg")
    oldpwd = os.getcwd()
    os.chdir(repo + "../../")
    s2 = packager.backtick(["apt-ftparchive", "release", "."])
    try:
        with open("Release", "wb") as fh:
            fh.write(s1.encode("utf-8"))
            fh.write(s2)
    finally:
        os.chdir(oldpwd)


def move_repos_into_place(src, dst):
    """Move the repos into place."""
    # Find all the stuff in src/*, move it to a freshly-created
    # directory beside dst, then play some games with symlinks so that
    # dst is a name the new stuff and dst+".old" names the previous
    # one.  This feels like a lot of hooey for something so trivial.

    # First, make a crispy fresh new directory to put the stuff in.
    idx = 0
    while True:
        date_suffix = time.strftime("%Y-%m-%d")
        dname = dst + ".%s.%d" % (date_suffix, idx)
        try:
            os.mkdir(dname)
            break
        except OSError:
            exc = sys.exc_info()[1]
            if exc.errno == errno.EEXIST:
                pass
            else:
                raise exc
        idx = idx + 1

    # Put the stuff in our new directory.
    for src_file in os.listdir(src):
        packager.sysassert(["cp", "-rv", src + "/" + src_file, dname])

    # Make a symlink to the new directory; the symlink will be renamed
    # to dst shortly.
    idx = 0
    while True:
        tmpnam = dst + ".TMP.%d" % idx
        try:
            os.symlink(dname, tmpnam)
            break
        except OSError:  # as exc: # Python >2.5
            exc = sys.exc_info()[1]
            if exc.errno == errno.EEXIST:
                pass
            else:
                raise exc
        idx = idx + 1

    # Make a symlink to the old directory; this symlink will be
    # renamed shortly, too.
    oldnam = None
    if os.path.exists(dst):
        idx = 0
        while True:
            oldnam = dst + ".old.%d" % idx
            try:
                os.symlink(os.readlink(dst), oldnam)
                break
            except OSError:  # as exc: # Python >2.5
                exc = sys.exc_info()[1]
                if exc.errno == errno.EEXIST:
                    pass
                else:
                    raise exc

    os.rename(tmpnam, dst)
    if oldnam:
        os.rename(oldnam, dst + ".old")


if __name__ == "__main__":
    main()
