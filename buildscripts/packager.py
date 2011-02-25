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
import httplib
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
# binaries.  The packaging
BINARYDIR="BINARIES"

def main(argv):
    (versions, suffixes) = parse_args(argv[1:])

    srcdir=os.getcwd()+"/../"

    # We do all our work in a randomly-created directory. You can set
    # TEMPDIR to influence where this program will do stuff.
    prefix=tempfile.mkdtemp()
    print "Working in directory %s" % prefix
    os.chdir(prefix)

    # This will be a list of directories where we put packages in
    # "repository layout".
    repos=[]

    # Download the binaries.
    urlfmt="http://fastdl.mongodb.org/linux/mongodb-linux-%s-%s.tgz"
    for (version, arch) in crossproduct(versions, ARCHES):
        httpget(urlfmt % (arch, version), ensure_dir(tarfile(arch, version)))

    # Build a pacakge for each distro/version/arch tuple, and
    # accumulate the repository-layout directories.
    for (distro, version, arch) in crossproduct(DISTROS, versions, ARCHES):
        repos.append(make_package(srcdir, arch, distro, version, suffixes))

    # Build the repos' metadatas.
    for repo in set(repos):
        make_repo(repo)

    move_repos_into_place(os.getcwd()+"/repo", REPOPATH)

    # FIXME: try shutil.rmtree some day.
    sysassert(["rm", "-rv", prefix])

def parse_args(args):
    if len(args) == 0:
        print """Usage: packager.py SPEC1 SPEC2 ... SPECn

Each SPEC is a mongodb version string optionally followed by a colon
and a "suffix" to append to the package's base name.  (If unsupplied,
suffixes default based on the parity of the middle number in the
version.)"""
        sys.exit(0)
    # Parse each argument as a version number with an optional
    # colon-delimited suffix to use for the package name.  (The
    # suffixes default to "-10gen" and "-10gen-unstable" based on the
    # parity of the middle number in the version, but we want
    # 1.8.0-rc0 would be mongodb-10gen-unstable, even though the
    # middle version is even, so this optional explicit suffix forces
    # that.)
    versions=[]
    suffixes={}
    for arg in args:
        tup = arg.split(":")
        versions.append(tup[0])
        if len(tup)>1:
            suffixes[tup[0]]=tup[1]
    return (versions, suffixes)

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


def tarfile(arch, version):
    """Return the location where we store the downloaded tarball for
    (arch, version)"""
    return "dl/mongodb-linux-%s-%s.tar.gz" % (version, arch)

def repodir(distro, distro_arch):
    """Return the directory where we'll place the package files for
    (distro, distro_version) in that distro's preferred repository
    layout (as distinct from where that distro's packaging building
    tools place the package files)."""
    if re.search("^(debian|ubuntu)", distro):
        return "repo/%s/dists/dist/10gen/binary-%s/" % (distro, distro_arch)
    elif re.search("(redhat|fedora|centos)", distro):
        return "repo/%s/os/%s/RPMS/" % (distro, distro_arch)
    else:
        raise Exception("BUG: unsupported platform?")

def httpget(url, filename):
    """Download the contents of url to filename, return filename."""
    print "Fetching %s to %s." % (url, filename)
    conn = None
    u=urlparse.urlparse(url)
    assert(u.scheme=='http')
    try:
        conn = httplib.HTTPConnection(u.hostname)
        conn.request("GET", u.path)
        t=filename+'.TMP'
        res = conn.getresponse()
        # FIXME: follow redirects
        if res.status==200:
            f = open(t, 'w')
            try:
                f.write(res.read())
            finally:
                f.close()
                
        else:
            raise Exception("HTTP error %d" % res.status)
        os.rename(t, filename)
    finally:
        if conn:
            conn.close()
    return filename

def unpack_binaries_into(arch, version, where):
    """Unpack the tarfile for (arch, version) into directory where."""
    rootdir=os.getcwd()
    ensure_dir(where)
    # Note: POSIX tar doesn't require support for gtar's "-C" option,
    # and Python's tarfile module prior to Python 2.7 doesn't have the
    # features to make this detail easy.  So we'll just do the dumb
    # thing and chdir into where and run tar there.
    os.chdir(where)
    try:
        sysassert(["tar", "xvzf", rootdir+"/"+tarfile(arch, version), "mongodb-linux-%s-%s/bin" % (arch, version)])
        os.rename("mongodb-linux-%s-%s/bin" % (arch, version), "bin")
        os.rmdir("mongodb-linux-%s-%s" % (arch, version))
    except Exception:
        exc=sys.exc_value
        os.chdir(rootdir)
        raise exc
    os.chdir(rootdir)

def make_package(srcdir, arch, distro, version, suffixes):
    """Construct the package for (arch, distro, version), getting
    packaging files from srcdir and any user-specified suffix from
    suffixes"""
    # Note: Debian packages have funny rules about dashes
    # in version numbers, and RPM simply forbids dashes.
    # pversion will be the package's version number (but
    # we need to know our upstream version too).
    if re.search("^(debian|ubuntu)", distro):
        pversion=re.sub("-", "~", version)                
    elif re.search("(redhat|fedora|centos)", distro):
        pversion=re.sub("\\d+-", "", version)
    else:
        raise Exception("BUG: unsupported platform?")

    # pkgbase is the first part of the package's name on
    # this distro.
    pkgbase="mongo" if re.search("(redhat|fedora|centos)", distro) else "mongodb"
    # suffix is what we tack on after pkgbase.
    suffix=suffixes[version] if version in suffixes else ("-10gen" if int(version.split(".")[1])%2==0 else "-10gen-unstable")

    # The setupdir will be a directory containing all inputs to the
    # distro's packaging tools (e.g., package metadata files, init
    # scripts, etc), along with the already-built binaries).  In case
    # the following format string is unclear, an example setupdir
    # would be dst/x86_64/debian-sysvinit/mongodb-10gen-unstable/
    setupdir="dst/%s/%s/%s%s-%s/" % (arch, distro, pkgbase, suffix, pversion)
    ensure_dir(setupdir)
    # Note that the RPM packages get their man pages from the debian
    # directory, so the debian directory is needed in all cases (and
    # innocuous in the debianoids' setupdirs).
    for pkgdir in ["debian", "rpm"]:
        print "Copying packaging files from %s to %s" % ("%s/%s" % (srcdir, pkgdir), setupdir)
        # FIXME: investigate whether shutil.copytree does this job as
        # effectively and without agonizing edge cases.
        sysassert(["cp", "-r", "%s/%s" % (srcdir, pkgdir), setupdir])
    # Splat the binaries under setupdir.  The "build" stages of the
    # packaging infrastructure will move the binaries to wherever they
    # need to go.  
    unpack_binaries_into(arch, version, setupdir+("%s/usr/"%BINARYDIR))
    # Remove the mongosniff binary due to libpcap dynamic
    # linkage.  FIXME: this removal should go away
    # eventually.
    os.unlink(setupdir+("%s/usr/bin/mongosniff"%BINARYDIR))
    if re.search("^(debian|ubuntu)", distro):
        return make_deb(setupdir, distro, arch, pkgbase, suffix, pversion)
    elif re.search("^(centos|redhat|fedora)", distro):
        return make_rpm(setupdir, distro, arch, pkgbase, suffix, pversion)
    else:
        raise Exception("BUG: unsupported platform?")

def make_repo(repodir):
    if re.search("(debian|ubuntu)", repodir):
        make_deb_repo(repodir)
    elif re.search("(centos|redhat|fedora)", repodir):
        make_rpm_repo(repodir)
    else:
        raise Exception("BUG: unsupported platform?")

def make_deb(dir, distro, arch, pkgbase, suffix, version):
    # remove the upstart file from the
    # sysvinit-flavored debianoids
    if re.search("sysvinit", distro):
        os.unlink(dir+"debian/mongodb.upstart")
    # Rewrite the control and rules files
    write_debian_control_file(dir+"debian/control", suffix)
    write_debian_rules_file(dir+"debian/rules", suffix)
    # FIXME: use real changelogs.
    write_bogus_debian_changelog(dir+"debian/changelog", suffix, version)
    distro_arch="i386" if arch.endswith("86") else "amd64"
    # Do the packaging.
    oldcwd=os.getcwd()
    try:
        os.chdir(dir)
        sysassert(["dpkg-buildpackage", "-a"+distro_arch])
    finally:
        os.chdir(oldcwd)
    r=repodir(distro, distro_arch)
    ensure_dir(r)
    # FIXME: see if shutil.copyfile or something can do this without
    # much pain.
    sysassert(["cp", "-v", dir+"../%s%s_%s_%s.deb"%(pkgbase, suffix, version, distro_arch), r])
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


def write_bogus_debian_changelog(path, suffix, version):
    s="""mongodb%s (%s) unstable; urgency=low

  * sharding lots of changes
  * replica_sets lots of changes

 -- Richard Kreuter <richard@10gen.com>  Mon, 20 Dec 2010 16:56:28 -0500

""" % (suffix, version)
    f=open(path, 'w')
    try:
        f.write(s)
    finally:
        f.close()

def write_debian_control_file(path, suffix):
    s="""Source: @@PACKAGE_BASENAME@@
Section: devel
Priority: optional
Maintainer: Richard Kreuter <richard@10gen.com>
Build-Depends: 
Standards-Version: 3.8.0
Homepage: http://www.mongodb.org

Package: @@PACKAGE_BASENAME@@
Conflicts: @@PACKAGE_CONFLICTS@@
Architecture: any
Depends: libc6 (>= 2.3.2), libgcc1 (>= 1:4.1.1), libstdc++6 (>= 4.1.1)
Description: An object/document-oriented database
 MongoDB is a high-performance, open source, schema-free 
 document-oriented  data store that's easy to deploy, manage
 and use. It's network accessible, written in C++ and offers
 the following features :
 .
    * Collection oriented storage - easy storage of object-
      style data
    * Full index support, including on inner objects
    * Query profiling
    * Replication and fail-over support
    * Efficient storage of binary data including large 
      objects (e.g. videos)
    * Auto-sharding for cloud-level scalability (Q209)
 .
 High performance, scalability, and reasonable depth of
 functionality are the goals for the project.
"""
    s=re.sub("@@PACKAGE_BASENAME@@", "mongodb%s" % suffix, s)
    conflict_suffixes=["", "-stable", "-unstable", "-nightly", "-10gen", "-10gen-unstable"]
    conflict_suffixes.remove(suffix)
    s=re.sub("@@PACKAGE_CONFLICTS@@", ", ".join(["mongodb"+suffix for suffix in conflict_suffixes]), s)
    f=open(path, 'w')
    try:
        f.write(s)
    finally:
        f.close()

def write_debian_rules_file(path, suffix):
    # Note debian/rules is a makefile, so for visual disambiguation we
    # make all tabs here \t.
    s="""#!/usr/bin/make -f
# -*- makefile -*-
# Sample debian/rules that uses debhelper.
# This file was originally written by Joey Hess and Craig Small.
# As a special exception, when this file is copied by dh-make into a
# dh-make output file, you may use that output file without restriction.
# This special exception was added by Craig Small in version 0.37 of dh-make.

# Uncomment this to turn on verbose mode.
#export DH_VERBOSE=1


configure: configure-stamp
configure-stamp:
\tdh_testdir
        # Add here commands to configure the package.

\ttouch configure-stamp


build: build-stamp

build-stamp: configure-stamp  
\tdh_testdir

        # Add here commands to compile the package.
# THE FOLLOWING LINE IS INTENTIONALLY COMMENTED. 
\t# scons 
        #docbook-to-man debian/mongodb.sgml > mongodb.1
\tls debian/*.1 > debian/mongodb.manpages

\ttouch $@

clean: 
\tdh_testdir
\tdh_testroot
\trm -f build-stamp configure-stamp

\t# FIXME: scons freaks out at the presence of target files
\t# under debian/mongodb.
\t#scons -c
\trm -rf $(CURDIR)/debian/@@PACKAGE_BASENAME@@
\trm -f config.log
\trm -f mongo
\trm -f mongod
\trm -f mongoimportjson
\trm -f mongoexport
\trm -f mongorestore
\trm -f mongodump
\trm -f mongofiles
\trm -f .sconsign.dblite
\trm -f libmongoclient.a
\trm -rf client/*.o
\trm -rf tools/*.o
\trm -rf shell/*.o
\trm -rf .sconf_temp
\trm -f buildscripts/*.pyc 
\trm -f *.pyc
\trm -f buildinfo.cpp
\tdh_clean debian/files

install: build
\tdh_testdir
\tdh_testroot
\tdh_prep
\tdh_installdirs

# THE FOLLOWING LINE IS INTENTIONALLY COMMENTED.
\t# scons --prefix=$(CURDIR)/debian/mongodb/usr install
\tcp -v $(CURDIR)/@@BINARYDIR@@/usr/bin/* $(CURDIR)/debian/@@PACKAGE_BASENAME@@/usr/bin
\tmkdir -p $(CURDIR)/debian/@@PACKAGE_BASENAME@@/etc
\tcp $(CURDIR)/debian/mongodb.conf $(CURDIR)/debian/@@PACKAGE_BASENAME@@/etc/mongodb.conf 

\tmkdir -p $(CURDIR)/debian/@@PACKAGE_BASENAME@@/usr/share/lintian/overrides/
\tinstall -m 644 $(CURDIR)/debian/lintian-overrides \
\t\t$(CURDIR)/debian/@@PACKAGE_BASENAME@@/usr/share/lintian/overrides/@@PACKAGE_BASENAME@@

# Build architecture-independent files here.
binary-indep: build install
# We have nothing to do by default.

# Build architecture-dependent files here.
binary-arch: build install
\tdh_testdir
\tdh_testroot
\tdh_installchangelogs 
\tdh_installdocs
\tdh_installexamples
#\tdh_install
#\tdh_installmenu
#\tdh_installdebconf\t
#\tdh_installlogrotate
#\tdh_installemacsen
#\tdh_installpam
#\tdh_installmime
\tdh_installinit
#\tdh_installinfo
\tdh_installman
\tdh_link
\tdh_strip
\tdh_compress
\tdh_fixperms
\tdh_installdeb
\tdh_shlibdeps
\tdh_gencontrol
\tdh_md5sums
\tdh_builddeb

binary: binary-indep binary-arch
.PHONY: build clean binary-indep binary-arch binary install configure
"""
    s=re.sub("@@PACKAGE_BASENAME@@", "mongodb%s" % suffix, s)
    s=re.sub("@@BINARYDIR@@", BINARYDIR, s)
    f=open(path, 'w')
    try:
        f.write(s)
    finally:
        f.close()
    # FIXME: some versions of debianoids seem to
    # need the rules file to be 755?
    os.chmod(path, stat.S_IXUSR|stat.S_IWUSR|stat.S_IRUSR|stat.S_IXGRP|stat.S_IRGRP|stat.S_IXOTH|stat.S_IWOTH)

def make_rpm(dir, distro, arch, pkgbase, suffix, version):
    # Create the specfile.
    specfile=dir+"rpm/mongo%s.spec" % suffix
    write_rpm_spec_file(specfile, suffix, version)
    topdir=ensure_dir(os.getcwd()+'/rpmbuild/')
    for subdir in ["BUILD", "RPMS", "SOURCES", "SPECS", "SRPMS"]:
        ensure_dir("%s/%s/" % (topdir, subdir))
    distro_arch="i686" if arch.endswith("86") else "x86_64"
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
    os.chdir(dir+"/../")
    try:
        sysassert(["tar", "-cpzf", topdir+"SOURCES/mongo%s-%s.tar.gz" % (suffix, version), os.path.basename(os.path.dirname(dir))])
    finally:
        os.chdir(oldcwd)
    # Do the build.
    sysassert(["rpmbuild", "-ba", "--target", distro_arch] + flags + ["%s/SPECS/mongo%s.spec" % (topdir, suffix)])
    r=repodir(distro, distro_arch)
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

def write_rpm_spec_file(path, suffix, version):
    s="""Name: @@PACKAGE_BASENAME@@
Conflicts: @@PACKAGE_CONFLICTS@@
Obsoletes: @@PACKAGE_OBSOLETES@@
Version: @@PACKAGE_VERSION@@
Release: mongodb_1%{?dist}
Summary: mongo client shell and tools
License: AGPL 3.0
URL: http://www.mongodb.org
Group: Applications/Databases

Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

%description
Mongo (from "huMONGOus") is a schema-free document-oriented database.
It features dynamic profileable queries, full indexing, replication
and fail-over support, efficient storage of large binary data objects,
and auto-sharding.

This package provides the mongo shell, import/export tools, and other
client utilities.

%package server
Summary: mongo server, sharding server, and support scripts
Group: Applications/Databases
Requires: @@PACKAGE_BASENAME@@

%description server
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo server software, mongo sharding server
softwware, default configuration files, and init.d scripts.

%package devel
Summary: Headers and libraries for mongo development. 
Group: Applications/Databases

%description devel
Mongo (from "huMONGOus") is a schema-free document-oriented database.

This package provides the mongo static library and header files needed
to develop mongo client software.

%prep
%setup

%build
#scons --prefix=$RPM_BUILD_ROOT/usr all
# XXX really should have shared library here

%install
#scons --prefix=$RPM_BUILD_ROOT/usr install
mkdir -p $RPM_BUILD_ROOT/usr
cp -rv @@BINARYDIR@@/usr/bin $RPM_BUILD_ROOT/usr
mkdir -p $RPM_BUILD_ROOT/usr/share/man/man1
cp debian/*.1 $RPM_BUILD_ROOT/usr/share/man/man1/
mkdir -p $RPM_BUILD_ROOT/etc/rc.d/init.d
cp rpm/init.d-mongod $RPM_BUILD_ROOT/etc/rc.d/init.d/mongod
chmod a+x $RPM_BUILD_ROOT/etc/rc.d/init.d/mongod
mkdir -p $RPM_BUILD_ROOT/etc
cp rpm/mongod.conf $RPM_BUILD_ROOT/etc/mongod.conf
mkdir -p $RPM_BUILD_ROOT/etc/sysconfig
cp rpm/mongod.sysconfig $RPM_BUILD_ROOT/etc/sysconfig/mongod
mkdir -p $RPM_BUILD_ROOT/var/lib/mongo
mkdir -p $RPM_BUILD_ROOT/var/log/mongo
touch $RPM_BUILD_ROOT/var/log/mongo/mongod.log

%clean
#scons -c
rm -rf $RPM_BUILD_ROOT

%pre server
if ! /usr/bin/id -g mongod &>/dev/null; then
    /usr/sbin/groupadd -r mongod
fi
if ! /usr/bin/id mongod &>/dev/null; then
    /usr/sbin/useradd -M -r -g mongod -d /var/lib/mongo -s /bin/false \
	-c mongod mongod > /dev/null 2>&1
fi

%post server
if test $1 = 1
then
  /sbin/chkconfig --add mongod
fi

%preun server
if test $1 = 0
then
  /sbin/chkconfig --del mongod
fi

%postun server
if test $1 -ge 1
then
  /sbin/service mongod condrestart >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root,-)
#%doc README GNU-AGPL-3.0.txt

%{_bindir}/mongo
%{_bindir}/mongodump
%{_bindir}/mongoexport
%{_bindir}/mongofiles
%{_bindir}/mongoimport
%{_bindir}/mongorestore
%{_bindir}/mongostat
%{_bindir}/bsondump

%{_mandir}/man1/mongo.1*
%{_mandir}/man1/mongod.1*
%{_mandir}/man1/mongodump.1*
%{_mandir}/man1/mongoexport.1*
%{_mandir}/man1/mongofiles.1*
%{_mandir}/man1/mongoimport.1*
%{_mandir}/man1/mongosniff.1*
%{_mandir}/man1/mongostat.1*
%{_mandir}/man1/mongorestore.1*

%files server
%defattr(-,root,root,-)
%config(noreplace) /etc/mongod.conf
%{_bindir}/mongod
%{_bindir}/mongos
#%{_mandir}/man1/mongod.1*
%{_mandir}/man1/mongos.1*
/etc/rc.d/init.d/mongod
/etc/sysconfig/mongod
#/etc/rc.d/init.d/mongos
%attr(0755,mongod,mongod) %dir /var/lib/mongo
%attr(0755,mongod,mongod) %dir /var/log/mongo
%attr(0640,mongod,mongod) %config(noreplace) %verify(not md5 size mtime) /var/log/mongo/mongod.log

%changelog
* Thu Jan 28 2010 Richard M Kreuter <richard@10gen.com>
- Minor fixes.

* Sat Oct 24 2009 Joe Miklojcik <jmiklojcik@shopwiki.com> - 
- Wrote mongo.spec.
"""
    s=re.sub("@@PACKAGE_BASENAME@@", "mongo%s" % suffix, s)
    s=re.sub("@@PACKAGE_VERSION@@", version, s)
    s=re.sub("@@BINARYDIR@@", BINARYDIR, s)
    conflict_suffixes=["", "-10gen", "-10gen-unstable"]
    conflict_suffixes.remove(suffix)
    s=re.sub("@@PACKAGE_CONFLICTS@@", ", ".join(["mongo"+_ for _ in conflict_suffixes]), s)
    if suffix == "-10gen":
        s=re.sub("@@PACKAGE_PROVIDES@@", "mongo-stable", s)
        s=re.sub("@@PACKAGE_OBSOLETES@@", "mongo-stable", s)
    elif suffix == "-10gen-unstable":
        s=re.sub("@@PACKAGE_PROVIDES@@", "mongo-unstable", s)
        s=re.sub("@@PACKAGE_OBSOLETES@@", "mongo-unstable", s)
    else:
        raise Exception("BUG: unknown suffix %s" % suffix)

    f=open(path, 'w')
    try:
        f.write(s)
    finally:
        f.close()

if __name__ == "__main__":
    main(sys.argv)



