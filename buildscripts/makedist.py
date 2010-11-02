#!/usr/bin/env python

# makedist.py: make a distro package (on an EC2 (or sometimes
# RackSpace) instance)

# For ease of use, put a file called settings.py someplace in your
# sys.path, containing something like the following:

# makedist = {
#     # This gets supplied to EC2 to rig up an ssh key for
#     # the remote user.
#     "ec2_sshkey" : "key-id",
#     # And so we need to tell our ssh processes where to find the
#     # appropriate public key file.
#     "ssh_keyfile" : "/path/to/key-id-file"
#     }

# Notes: although there is a Python library for accessing EC2 as a web
# service, it seemed as if it would be less work to just shell out to
# the three EC2 management tools we use.

# To make a distribution we must:

# 1. Fire up an EC2 AMI suitable for building.
# 2. Get any build-dependencies and configurations onto the remote host.
# 3. Fetch the mongodb source.
# 4. Run the package building tools.
# 5. Save the package archives someplace permanent (eventually we
#    ought to install them into a public repository for the distro).
# Unimplemented:
# 6. Fire up an EC2 AMI suitable for testing whether the packages
#    install.
# 7. Check whether the packages install and run.

# The implementations of steps 1, 2, 4, 5, 6, and 7 will depend on the
# distro of host we're talking to (Ubuntu, CentOS, Debian, etc.).

from __future__ import with_statement
import subprocess
import sys
import signal
import getopt
import socket
import time
import os.path
import tempfile
import string
import settings

from libcloud.types import Provider
from libcloud.providers import get_driver
from libcloud.drivers.ec2 import EC2NodeDriver, NodeImage
from libcloud.base import Node, NodeImage, NodeSize, NodeState
from libcloud.ssh import ParamikoSSHClient

# For the moment, we don't handle any of the errors we raise, so it
# suffices to have a simple subclass of Exception that just
# stringifies according to a desired format.
class SimpleError(Exception):
    def __init__(self, *args):
        self.args = args
    def __str__(self):
        return self.args[0] % self.args[1:]

class SubcommandError(SimpleError):
    def __init__(self, *args):
        self.status = args[2]
        super(SubcommandError, self).__init__(*args)

class BaseConfigurator (object):
    def __init__ (self, **kwargs):
        self.configuration = []
        self.arch=kwargs["arch"]
        self.distro_name=kwargs["distro_name"]
        self.distro_version=kwargs["distro_version"]
        
    def lookup(self,  what, dist, vers, arch):
        for (wht, seq) in self.configuration:
            if what == wht:
                for ((dpat, vpat, apat), payload) in seq:
                    # For the moment, our pattern facility is just "*" or exact match.
                    if ((dist == dpat or dpat == "*") and
                        (vers == vpat or vpat == "*") and
                        (arch == apat or apat == "*")):
                        return payload
        if getattr(self, what, False):
            return getattr(self, what)
        else:
            raise SimpleError("couldn't find a%s %s configuration for dist=%s, version=%s, arch=%s",
                              "n" if ("aeiouAEIOU".find(what[0]) > -1) else "",
                              what, dist, vers, arch)

    def default(self, what):
        return self.lookup(what, self.distro_name, self.distro_version, self.arch)
    def findOrDefault(self, dict, what):
        return (dict[what] if what in dict else self.lookup(what, self.distro_name, self.distro_version, self.arch))

class BaseHostConfigurator (BaseConfigurator):
    def __init__(self, **kwargs):
        super(BaseHostConfigurator, self).__init__(**kwargs)
        self.configuration += [("distro_arch",
                               ((("debian", "*", "x86_64"), "amd64"),
                                (("ubuntu", "*", "x86_64"), "amd64"),
                                (("debian", "*", "x86"), "i386"),
                                (("ubuntu", "*", "x86"), "i386"),
                                (("centos", "*", "x86_64"), "x86_64"),
                                (("fedora", "*", "x86_64"), "x86_64"),
                                (("centos", "*", "x86"), "i386"),
                                (("fedora", "*", "x86"), "i386"),
                                (("*", "*", "x86_64"), "x86_64"),
                                (("*", "*", "x86"), "x86"))) ,
                              ]

class LocalHost(object):
    @classmethod
    def runLocally(cls, argv):
        print "running %s" % argv
        r = subprocess.Popen(argv).wait()
        if r != 0:
            raise SubcommandError("subcommand %s exited %d", argv, r)

class EC2InstanceConfigurator(BaseConfigurator):
    def __init__(self, **kwargs):
        super(EC2InstanceConfigurator, self).__init__(**kwargs)
        self.configuration += [("ec2_ami",
                                ((("ubuntu", "10.10", "x86_64"), "ami-688c7801"),
                                 (("ubuntu", "10.10", "x86"), "ami-1a837773"),
                                 (("ubuntu", "10.10", "x86"), "ami-508c7839"),
                                 (("ubuntu", "10.4", "x86_64"), "ami-bf07ead6"),
                                 (("ubuntu", "10.4", "x86"), "ami-f707ea9e"),
                                 (("ubuntu", "9.10", "x86_64"), "ami-55739e3c"),
                                 (("ubuntu", "9.10", "x86"), "ami-bb709dd2"),
                                 (("ubuntu", "9.4", "x86_64"), "ami-eef61587"),
                                 (("ubuntu", "9.4", "x86"), "ami-ccf615a5"),
                                 (("ubuntu", "8.10", "x86"), "ami-c0f615a9"),
                                 (("ubuntu", "8.10", "x86_64"), "ami-e2f6158b"),
                                 (("ubuntu", "8.4", "x86"), "ami59b35f30"),
                                 (("ubuntu", "8.4", "x86_64"), "ami-27b35f4e"),
                                 (("debian", "5.0", "x86"), "ami-dcf615b5"),
                                 (("debian", "5.0", "x86_64"), "ami-f0f61599"),
                                 (("centos", "5.4", "x86"), "ami-f8b35e91"),
                                 (("centos", "5.4", "x86_64"), "ami-ccb35ea5"),
                                 (("fedora", "8", "x86_64"), "ami-2547a34c"),
                                 (("fedora", "8", "x86"), "ami-5647a33f"))),
                               ("rackspace_imgname",
                                ((("fedora", "11", "x86_64"), "Fedora 11"),
                                 (("fedora", "12", "x86_64"), "Fedora 12"),
                                 (("fedora", "13", "x86_64"), "Fedora 13"))),
                               ("ec2_mtype",
                                ((("*", "*", "x86"), "m1.small"),
                                 (("*", "*", "x86_64"), "m1.large"))),
                               ]
    
class nodeWrapper(object):
    def __init__(self, configurator, **kwargs):
        self.terminate = False if "no_terminate" in kwargs else True
        self.use_internal_name = False

    def getHostname(self): 
        internal_name=self.node.private_ip[0]
        public_name=self.node.public_ip[0]
        if not (internal_name or external_name):
            raise Exception('host has no name?')
        if self.use_internal_name:
            # FIXME: by inspection, it seems this is sometimes the
            # empty string.  Dunno if that's EC2 or libcloud being
            # stupid, but it's not good.
            if internal_name:
                return internal_name
            else:
                return public_name
        else:
            return public_name 
    
    def initwait(self):
        print "waiting for node to spin up"
        # Wait for EC2 to tell us the node is running.
        while 1:
            n=None
            # EC2 sometimes takes a while to report a node.
            for i in range(6):
                nodes = [n for n in self.list_nodes() if (n.id==self.node.id)]
                if len(nodes)>0:
                    n=nodes[0]
                    break
                else:
                    time.sleep(10)
            if not n:
                raise Exception("couldn't find node with id %s" % self.node.id)
            if n.state == NodeState.PENDING: 
                time.sleep(10)
            else:
                self.node = n
                break
        print "ok"
        # Now wait for the node's sshd to be accepting connections.
        print "waiting for ssh"
        sshwait = True
        if sshwait == False:
            return
        while sshwait:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                try:
                    s.connect((self.node.public_ip[0], 22))
                    sshwait = False
                    print "connected on port 22 (ssh)"
                    time.sleep(15) # arbitrary timeout, in case the
                                  # remote sshd is slow.
                except socket.error, err:
                    pass
            finally:
                s.close()
                time.sleep(3) # arbitrary timeout
        print "ok"

    def __enter__(self):
        self.start()
        # Note: we don't do an initwait() in __enter__ because if an
        # exception is raised during __enter__, __exit__ doesn't get
        # run (and by inspection RackSpace doesn't let you kill a node
        # that hasn't finished booting yet).
        return self

    def __exit__(self, type, value, traceback):
        self.stop()

    def stop(self):
        if self.terminate:
            print "Destroying node %s" % self.node.id
            self.node.destroy()
        else:
            print "Not terminating EC2 instance %s." % self.node.id

    def setup(self):
        pass

class EC2Instance (nodeWrapper):
    def __init__(self, configurator, **kwargs):
        super(EC2Instance, self).__init__(configurator, **kwargs)
        # Stuff we need to start an instance: AMI name, key and cert
        # files.  AMI and mtype default to configuration in this file,
        # but can be overridden.
        self.ec2_ami   = configurator.findOrDefault(kwargs, "ec2_ami")
        self.ec2_mtype = configurator.findOrDefault(kwargs, "ec2_mtype")
        self.use_internal_name = True if "use_internal_name" in kwargs else False
        self.ec2_sshkey=kwargs["ec2_sshkey"]

        # FIXME: this needs to be a commandline option
        self.ec2_groups = ["default", "buildbot-slave", "dist-slave"]


    def start(self):
        "Fire up a fresh EC2 instance."
        EC2 = get_driver(Provider.EC2)
        self.driver = EC2NodeDriver(settings.id, settings.key)
        image = NodeImage(self.ec2_ami, self.ec2_ami, EC2)
        size = NodeSize(self.ec2_mtype, self.ec2_mtype, None, None, None, None, EC2)
        self.node = self.driver.create_node(image=image, name=self.ec2_ami, size=size, keyname=self.ec2_sshkey, securitygroup=self.ec2_groups)
        print "Created node %s" % self.node.id
    
    def list_nodes(self):
        return self.driver.list_nodes()
        
class SshConnectionConfigurator (BaseConfigurator):
    def __init__(self, **kwargs):
        super(SshConnectionConfigurator, self).__init__(**kwargs)
        self.configuration += [("ssh_login",
                                # FLAW: this actually depends more on the AMI
                                # than the triple.
                                ((("debian", "*", "*"), "root"),
                                 (("ubuntu", "10.10", "*"), "ubuntu"),
                                 (("ubuntu", "10.4", "*"), "ubuntu"),
                                 (("ubuntu", "9.10", "*"), "ubuntu"),
                                 (("ubuntu", "9.4", "*"), "root"),
                                 (("ubuntu", "8.10", "*"), "root"),
                                 (("ubuntu", "8.4", "*"), "ubuntu"),
                                 (("fedora", "*", "*"), "root"),
                                 (("centos", "*", "*"), "root"))),
                               ]

class SshConnection (object):
    def __init__(self, configurator, **kwargs):
        # Stuff we need to talk to the thing properly
        self.ssh_login = configurator.findOrDefault(kwargs, "ssh_login")

        self.ssh_host = kwargs["ssh_host"]
        self.ssh_keyfile=kwargs["ssh_keyfile"]
        # Gets set to False when we think we can ssh in.
        self.sshwait = True

    def initSsh(self):
        ctlpath="/tmp/ec2-ssh-%s-%s-%s" % (self.ssh_host, self.ssh_login, os.getpid())
        argv = ["ssh", "-o", "StrictHostKeyChecking no",
                "-M", "-o", "ControlPath %s" % ctlpath,
                "-v", "-l", self.ssh_login, "-i",  self.ssh_keyfile,
                self.ssh_host]
        print "Setting up ssh master connection with %s" % argv
        self.sshproc = subprocess.Popen(argv)
        self.ctlpath = ctlpath


    def __enter__(self):
        self.initSsh()
        return self
        
    def __exit__(self, type, value, traceback):
        os.kill(self.sshproc.pid, signal.SIGTERM)
        self.sshproc.wait()
        
    def runRemotely(self, argv):
        """Run a command on the host."""
        LocalHost.runLocally(["ssh", "-o", "StrictHostKeyChecking no",
                         "-S", self.ctlpath,
                         "-l", self.ssh_login,
                         "-i",  self.ssh_keyfile,
                         self.ssh_host] + argv)

    def sendFiles(self, files):
        for (localfile, remotefile) in files:
            LocalHost.runLocally(["scp", "-o", "StrictHostKeyChecking no",
                             "-o", "ControlMaster auto",
                             "-o", "ControlPath %s" % self.ctlpath,
                             "-i",  self.ssh_keyfile,
                             "-rv", localfile,
                             self.ssh_login + "@" + self.ssh_host + ":" +
                             ("" if remotefile is None else remotefile) ])

    def recvFiles(self, files):
        for (remotefile, localfile) in files:
            LocalHost.runLocally(["scp", "-o", "StrictHostKeyChecking no",
                             "-o", "ControlMaster auto",
                             "-o", "ControlPath %s" % self.ctlpath,
                             "-i",  self.ssh_keyfile,
                             "-rv", 
                             self.ssh_login + "@" + self.ssh_host +
                             ":" + remotefile,
                             "." if localfile is None else localfile ])


class ScriptFileConfigurator (BaseConfigurator):
    deb_productdir = "dists"
    rpm_productdir = "/usr/src/redhat/RPMS" # FIXME: this could be
                                            # ~/redhat/RPMS or
                                            # something elsewhere

    preamble_commands = """
set -x # verbose execution, for debugging
set -e # errexit, stop on errors
"""
    # Strictly speaking, we don't need to mangle debian files on rpm
    # systems (and vice versa), but (a) it doesn't hurt anything to do
    # so, and (b) mangling files the same way everywhere could
    # conceivably help uncover bugs in the hideous hideous sed
    # programs we're running here.  (N.B., for POSIX wonks: POSIX sed
    # doesn't support either in-place file editing, which we use
    # below.  So if we end up wanting to run these mangling commands
    # e.g., on a BSD, we'll need to make them fancier.)
    mangle_files_commands ="""
# On debianoids, the package names in the changelog and control file
# must agree, and only files in a subdirectory of debian/ matching the
# package name will get included in the .deb, so we also have to mangle
# the rules file.
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i '1s/.*([^)]*)/{pkg_name}{pkg_name_suffix} ({pkg_version})/' debian/changelog ) || exit 1
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's/^Source:.*/Source: {pkg_name}{pkg_name_suffix}/;
s/^Package:.*mongodb/Package: {pkg_name}{pkg_name_suffix}\\
Conflicts: {pkg_name_conflicts}/' debian/control; ) || exit 1
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's|$(CURDIR)/debian/mongodb/|$(CURDIR)/debian/{pkg_name}{pkg_name_suffix}/|g' debian/rules) || exit 1
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's|debian/mongodb.manpages|debian/{pkg_name}{pkg_name_suffix}.manpages|g' debian/rules) || exit 1
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i '/^Name:/s/.*/Name: {pkg_name}{pkg_name_suffix}\\
Conflicts: {pkg_name_conflicts}/; /^Version:/s/.*/Version: {pkg_version}/; /Requires.*mongo/s/mongo/{pkg_name}{pkg_name_suffix}/;' rpm/mongo.spec )
# Debian systems require some ridiculous workarounds to get an init
# script at /etc/init.d/mongodb when the packge name isn't the init
# script name.  Note: dh_installinit --name won't work, because that
# option would require the init script under debian/ to be named
# mongodb.
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" &&
ln debian/init.d debian/{pkg_name}{pkg_name_suffix}.mongodb.init &&
ln debian/mongodb.upstart debian/{pkg_name}{pkg_name_suffix}.mongodb.upstart &&
sed -i 's/dh_installinit/dh_installinit --name=mongodb/' debian/rules) || exit 1
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && cat debian/rules)
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && cat rpm/mongo.spec)
"""

    # If we're just packaging up nightlies, do this:
    nightly_build_mangle_files="""
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i '/scons[[:space:]]*$/d; s^scons.*install^mkdir -p debian/{pkg_name}{pkg_name_suffix} \&\& wget http://downloads.mongodb.org/linux/mongodb-linux-{mongo_arch}-{mongo_pub_version}.tgz \&\& tar xzvf mongodb-linux-{mongo_arch}-{mongo_pub_version}.tgz \&\& find `tar tzf mongodb-linux-{mongo_arch}-{mongo_pub_version}.tgz | sed "s|/.*||" | sort -u | head -n1` -mindepth 1 -maxdepth 1 -type d | xargs -n1 -IARG mv -v ARG debian/{pkg_name}{pkg_name_suffix}/usr \&\& (rm debian/{pkg_name}{pkg_name_suffix}/usr/bin/mongosniff || true)^' debian/rules)
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's/^BuildRequires:.*//; s/scons.*\ -c//; s/scons.*\ all//; s^scons.*install^(mkdir -p $RPM_BUILD_ROOT/usr ; cd /tmp \&\& curl http://downloads.mongodb.org/linux/mongodb-linux-{mongo_arch}-{mongo_pub_version}.tgz > mongodb-linux-{mongo_arch}-{mongo_pub_version}.tgz \&\& tar xzvf mongodb-linux-{mongo_arch}-{mongo_pub_version}.tgz \&\& find `tar tzf mongodb-linux-{mongo_arch}-{mongo_pub_version}.tgz | sed "s|/.*||" | sort -u | head -n1` -mindepth 1 -maxdepth 1 -type d | xargs -n1 -IARG cp -pRv ARG $RPM_BUILD_ROOT/usr \&\& (rm -r $RPM_BUILD_ROOT/usr/bin/mongosniff $RPM_BUILD_ROOT/usr/lib64/libmongoclient.a $RPM_BUILD_ROOT/usr/lib/libmongoclient.a $RPM_BUILD_ROOT/usr/include/mongo || true))^' rpm/mongo.spec)
# Upstream nightlies no longer contain libmongoclient.
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i '/%package devel/{{N;N;d;}}; /%description devel/{{N;N;N;N;N;d;}}; /%files devel/{{N;N;N;d;}};' rpm/mongo.spec )
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && cat debian/rules)
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && cat rpm/mongo.spec)
"""
#$RPM_BUILD_ROOT/usr/lib/libmongoclient.a  $RPM_BUILD_ROOT/usr/lib64/libmongoclient.a
    mangle_files_for_new_deb_xulrunner_commands = """
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's/xulrunner-dev/xulrunner-1.9.2-dev/g' debian/control )
"""

    mangle_files_for_ancient_redhat_commands = """
# Ancient RedHats ship with very old boosts and non-UTF8-aware js
# libraries, so we need to link statically to those.
( cd  "{pkg_name}{pkg_name_suffix}-{pkg_version}" && sed -i 's|^scons.*((inst)all)|scons --prefix=$RPM_BUILD_ROOT/usr --extralib=nspr4 --staticlib=boost_system-mt,boost_thread-mt,boost_filesystem-mt,boost_program_options-mt,js $1|' rpm/mongo.spec )
"""

    deb_prereq_commands = """
# Configure debconf to never prompt us for input.
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y {pkg_prereq_str}
"""

    deb_build_commands="""
mkdir -p "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}"
mkdir -p "{pkg_product_dir}/{distro_version}/10gen/source"
( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}"; debuild ) || exit 1
# Try installing it
dpkg -i {pkg_name}{pkg_name_suffix}*.deb
ps ax | grep mongo || {{ echo "no running mongo" >/dev/stderr; exit 1; }}
dpkg --remove $(for f in {pkg_name}{pkg_name_suffix}*.deb ; do echo ${{f%%_*}}; done)
dpkg --purge $(for f in {pkg_name}{pkg_name_suffix}*.deb ; do echo ${{f%%_*}}; done)
cp {pkg_name}{pkg_name_suffix}*.deb "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}"
cp {pkg_name}{pkg_name_suffix}*.dsc "{pkg_product_dir}/{distro_version}/10gen/source"
cp {pkg_name}{pkg_name_suffix}*.tar.gz "{pkg_product_dir}/{distro_version}/10gen/source"
dpkg-scanpackages "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}" /dev/null | gzip -9c > "{pkg_product_dir}/{distro_version}/10gen/binary-{distro_arch}/Packages.gz"
dpkg-scansources "{pkg_product_dir}/{distro_version}/10gen/source" /dev/null | gzip -9c > "{pkg_product_dir}/{distro_version}/10gen/source/Sources.gz"
"""
    centos_prereq_commands = """
rpm -Uvh http://download.fedora.redhat.com/pub/epel/5/{distro_arch}/epel-release-5-4.noarch.rpm
yum -y install {pkg_prereq_str}
"""
    fedora_prereq_commands = """
#rpm -Uvh http://download.fedora.redhat.com/pub/epel/5/{distro_arch}/epel-release-5-4.noarch.rpm
yum -y install {pkg_prereq_str}
"""
    rpm_build_commands="""
for d in BUILD  BUILDROOT  RPMS  SOURCES  SPECS  SRPMS; do mkdir -p {rpmbuild_dir}/$d; done
cp -v "{pkg_name}{pkg_name_suffix}-{pkg_version}/rpm/mongo.spec" {rpmbuild_dir}/SPECS/{pkg_name}{pkg_name_suffix}.spec
tar -cpzf {rpmbuild_dir}/SOURCES/"{pkg_name}{pkg_name_suffix}-{pkg_version}".tar.gz "{pkg_name}{pkg_name_suffix}-{pkg_version}"
rpmbuild -ba --target={distro_arch} {rpmbuild_dir}/SPECS/{pkg_name}{pkg_name_suffix}.spec
# FIXME: should install the rpms, check if mongod is running.
""" 
    # FIXME: this is clean, but adds 40 minutes or so to the build process.
    old_rpm_precommands = """
yum install -y bzip2-devel python-devel libicu-devel chrpath zlib-devel nspr-devel readline-devel ncurses-devel
# FIXME: this is just some random URL found on rpmfind some day in 01/2010.
wget ftp://194.199.20.114/linux/EPEL/5Client/SRPMS/js-1.70-8.el5.src.rpm
rpm -ivh js-1.70-8.el5.src.rpm
sed -i 's/XCFLAGS.*$/XCFLAGS=\"%{{optflags}} -fPIC -DJS_C_STRINGS_ARE_UTF8\" \\\\/' /usr/src/redhat/SPECS/js.spec
rpmbuild -ba /usr/src/redhat/SPECS/js.spec
rpm -Uvh /usr/src/redhat/RPMS/{distro_arch}/js-1.70-8.{distro_arch}.rpm
rpm -Uvh /usr/src/redhat/RPMS/{distro_arch}/js-devel-1.70-8.{distro_arch}.rpm
# FIXME: this is just some random URL found on rpmfind some day in 01/2010.
wget ftp://195.220.108.108/linux/sourceforge/g/project/gr/gridiron2/support-files/FC10%20source%20RPMs/boost-1.38.0-1.fc10.src.rpm
rpm -ivh boost-1.38.0-1.fc10.src.rpm
rpmbuild -ba /usr/src/redhat/SPECS/boost.spec
rpm -ivh /usr/src/redhat/RPMS/{distro_arch}/boost-1.38.0-1.{distro_arch}.rpm
rpm -ivh /usr/src/redhat/RPMS/{distro_arch}/boost-devel-1.38.0-1.{distro_arch}.rpm
"""

    # This horribleness is an attempt to work around ways that you're
    # not really meant to package things for Debian unless you are
    # Debian.

    # On very old Debianoids, libboost-<foo>-dev will be some old
    # boost that's not as thready as we want, but which Eliot says
    # will work; on very new Debianoids, libbost-<foo>-dev is what we
    # want.
    unversioned_deb_boost_prereqs =  ["libboost-thread-dev", "libboost-filesystem-dev", "libboost-program-options-dev", "libboost-date-time-dev", "libboost-dev"]
    # On some in-between Debianoids, libboost-<foo>-dev is still a
    # 1.34, but 1.35 packages are available, so we want those.
    versioned_deb_boost_prereqs =  ["libboost-thread1.35-dev", "libboost-filesystem1.35-dev", "libboost-program-options1.35-dev", "libboost-date-time1.35-dev", "libboost1.35-dev"]

    new_versioned_deb_boost_prereqs =  ["libboost-thread1.42-dev", "libboost-filesystem1.42-dev", "libboost-program-options1.42-dev", "libboost-date-time1.42-dev", "libboost1.42-dev"]
    unversioned_deb_xulrunner_prereqs = ["xulrunner-dev"]

    old_versioned_deb_xulrunner_prereqs = ["xulrunner-1.9-dev"]
    new_versioned_deb_xulrunner_prereqs = ["xulrunner-1.9.2-dev"]

    common_deb_prereqs = [ "build-essential", "dpkg-dev", "libreadline-dev", "libpcap-dev", "libpcre3-dev", "git-core", "scons", "debhelper", "devscripts", "git-core" ]

    centos_preqres = ["js-devel", "readline-devel", "pcre-devel", "gcc-c++", "scons", "rpm-build", "git" ]
    fedora_prereqs = ["js-devel", "readline-devel", "pcre-devel", "gcc-c++", "scons", "rpm-build", "git", "curl" ]

    def __init__(self, **kwargs):
        super(ScriptFileConfigurator, self).__init__(**kwargs)
        # FIXME: this method is disabled until we get back around to
        # actually building from source.
        if None: # kwargs["mongo_version"][0] == 'r':
            self.get_mongo_commands = """
wget -Otarball.tgz "http://github.com/mongodb/mongo/tarball/{mongo_version}";
tar xzf tarball.tgz
mv "`tar tzf tarball.tgz | sed 's|/.*||' | sort -u | head -n1`" "{pkg_name}{pkg_name_suffix}-{pkg_version}"
"""
        else: 
            self.get_mongo_commands = """
git clone git://github.com/mongodb/mongo.git
"""
            # This is disabled for the moment.  it's for building the
            # tip of some versioned branch.
            if None: #kwargs['mongo_version'][0] == 'v':
                self.get_mongo_commands +="""
( cd mongo && git archive --prefix="{pkg_name}{pkg_name_suffix}-{pkg_version}/" "`git log origin/{mongo_version} | sed -n '1s/^commit //p;q'`" ) | tar xf -
"""
            else:
                self.get_mongo_commands += """
( cd mongo && git archive --prefix="{pkg_name}{pkg_name_suffix}-{pkg_version}/" "{mongo_version}" ) | tar xf -
"""

        if "local_mongo_dir" in kwargs:
            self.mangle_files_commands = """( cd "{pkg_name}{pkg_name_suffix}-{pkg_version}" && rm -rf debian rpm && cp -pvR ~/pkg/* . )
""" + self.mangle_files_commands

        self.configuration += [("pkg_product_dir",
                                ((("ubuntu", "*", "*"), self.deb_productdir),
                                 (("debian", "*", "*"), self.deb_productdir),
                                 (("fedora", "*", "*"), "~/rpmbuild/RPMS"),
                                 (("centos", "*", "*"), "/usr/src/redhat/RPMS"))),
                               ("pkg_prereqs",
                                ((("ubuntu", "9.4", "*"),
                                  self.versioned_deb_boost_prereqs + self.unversioned_deb_xulrunner_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "9.10", "*"),
                                  self.unversioned_deb_boost_prereqs + self.unversioned_deb_xulrunner_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "10.10", "*"),
                                  self.new_versioned_deb_boost_prereqs + self.new_versioned_deb_xulrunner_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "10.4", "*"),
                                  self.unversioned_deb_boost_prereqs + self.new_versioned_deb_xulrunner_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "8.10", "*"),
                                  self.versioned_deb_boost_prereqs + self.unversioned_deb_xulrunner_prereqs + self.common_deb_prereqs),
                                 (("ubuntu", "8.4", "*"),
                                  self.unversioned_deb_boost_prereqs + self.old_versioned_deb_xulrunner_prereqs + self.common_deb_prereqs),
                                 (("debian", "5.0", "*"),
                                  self.versioned_deb_boost_prereqs + self.unversioned_deb_xulrunner_prereqs + self.common_deb_prereqs),
                                 (("fedora", "*", "*"),
                                  self.fedora_prereqs),
                                 (("centos", "5.4", "*"),
                                  self.centos_preqres))),
                               # FIXME: this is deprecated
                               ("commands",
                                ((("debian", "*", "*"),
                                  self.deb_prereq_commands + self.get_mongo_commands + self.mangle_files_commands + self.deb_build_commands),
                                 (("ubuntu", "10.4", "*"),
                                  self.preamble_commands + self.deb_prereq_commands + self.get_mongo_commands + self.mangle_files_commands  + self.mangle_files_for_new_deb_xulrunner_commands + self.deb_build_commands),
                                  (("ubuntu", "*", "*"),
                                  self.preamble_commands + self.deb_prereq_commands + self.get_mongo_commands + self.mangle_files_commands + self.deb_build_commands),
                                 (("centos", "*", "*"),
                                  self.preamble_commands + self.old_rpm_precommands + self.centos_prereq_commands + self.get_mongo_commands + self.mangle_files_commands  + self.mangle_files_for_ancient_redhat_commands + self.rpm_build_commands),
                                 (("fedora", "*", "*"),
                                  self.preamble_commands + self.old_rpm_precommands + self.fedora_prereq_commands + self.get_mongo_commands + self.mangle_files_commands + self.rpm_build_commands))),
                               ("preamble_commands",
                                ((("*", "*", "*"), self.preamble_commands),
                                 )),
                               ("install_prereqs",
                                ((("debian", "*", "*"), self.deb_prereq_commands),
                                 (("ubuntu", "*", "*"), self.deb_prereq_commands),
                                 (("centos", "*", "*"), self.centos_prereq_commands),
                                 (("fedora", "*", "*"), self.fedora_prereq_commands))),
                               ("get_mongo",
                                ((("*", "*", "*"), self.get_mongo_commands),
                                 )),
                               ("mangle_mongo",
                                ((("debian", "*", "*"), self.mangle_files_commands),
                                 (("ubuntu", "10.10", "*"),
                                  self.mangle_files_commands  + self.mangle_files_for_new_deb_xulrunner_commands),
                                 (("ubuntu", "10.4", "*"),
                                  self.mangle_files_commands  + self.mangle_files_for_new_deb_xulrunner_commands),
                                 (("ubuntu", "*", "*"), self.mangle_files_commands),
                                 (("centos", "*", "*"),
                                  self.mangle_files_commands  + self.mangle_files_for_ancient_redhat_commands),
                                 (("fedora", "*", "*"),
                                  self.mangle_files_commands))),
                               ("build_prerequisites",
                                ((("fedora", "*", "*"), self.old_rpm_precommands),
                                 (("centos", "*", "*"), self.old_rpm_precommands),
                                 (("*", "*", "*"), ''))),
                               ("install_for_packaging",
                                ((("debian", "*", "*"),""),
                                 (("ubuntu", "*", "*"),""),
                                 (("fedora", "*", "*"), ""),
                                 (("centos", "*", "*"),""))),
                               ("build_package",
                                ((("debian", "*", "*"),
                                  self.deb_build_commands),
                                 (("ubuntu", "*", "*"),
                                  self.deb_build_commands),
                                 (("fedora", "*", "*"),
                                  self.rpm_build_commands),
                                 (("centos", "*", "*"),
                                  self.rpm_build_commands))),
                               ("pkg_name",
                                ((("debian", "*", "*"), "mongodb"),
                                 (("ubuntu", "*", "*"), "mongodb"),
                                 (("centos", "*", "*"), "mongo"),
                                 (("fedora", "*", "*"), "mongo"))),
                               # FIXME: there should be a command-line argument for this.
                               ("pkg_name_conflicts",
                                ((("*", "*", "*"),  ["", "-stable", "-unstable", "-snapshot", "-oldstable"]),
                                 )),
                               ("rpmbuild_dir",
                                ((("fedora", "*", "*"), "~/rpmbuild"),
                                 (("centos", "*", "*"), "/usr/src/redhat"),
                                 (("*", "*","*"), ''),
                                 )),
                                ]




class ScriptFile(object):
    def __init__(self, configurator, **kwargs):
        self.configurator = configurator
        self.mongo_version_spec = kwargs['mongo_version_spec']
        self.mongo_arch       = kwargs["arch"] if kwargs["arch"] == "x86_64" else "i686"
        self.pkg_prereqs        = configurator.default("pkg_prereqs")
        self.pkg_name           = configurator.default("pkg_name")
        self.pkg_product_dir    = configurator.default("pkg_product_dir")
        #self.formatter          = configurator.default("commands")
        self.distro_name        = configurator.default("distro_name")
        self.distro_version     = configurator.default("distro_version")
        self.distro_arch        = configurator.default("distro_arch")

    def bogoformat(self, fmt, **kwargs):
        r = ''
        i = 0
        while True:
            c = fmt[i]
            if c in '{}':
                i+=1
                c2=fmt[i]
                if c2 == c:
                    r+=c
                else:
                    j=i
                    while True:
                        p=fmt[j:].find('}')
                        if p == -1:
                            raise Exception("malformed format string starting at %d: no closing brace" % i)
                        else:
                            j+=p
                            if len(fmt) > (j+1) and fmt[j+1]=='}':
                                j+=2
                            else:
                                break
                    key = fmt[i:j]
                    r+=kwargs[key]
                    i=j
            else:
                r+=c
            i+=1
            if i==len(fmt):
                return r
    
    def fmt(self, formatter, **kwargs):
        try:
            return string.Formatter.format(formatter, kwargs)
        finally:
            return self.bogoformat(formatter, **kwargs)

    def genscript(self):
        script=''
        formatter = self.configurator.default("preamble_commands") + self.configurator.default("install_prereqs")
        script+=self.fmt(formatter,
                         distro_name=self.distro_name,
                         distro_version=self.distro_version,
                         distro_arch=self.distro_arch,
                         pkg_name=self.pkg_name,
                         pkg_product_dir=self.pkg_product_dir,
                         mongo_arch=self.mongo_arch,
                         pkg_prereq_str=" ".join(self.pkg_prereqs),
                         )

        specs=self.mongo_version_spec.split(',')
        for spec in specs:
            (version, pkg_name_suffix, pkg_version) = parse_mongo_version_spec(spec)
            mongo_version       = version if version[0] != 'n' else ('HEAD' if version == 'nlatest' else 'r'+version[1:]) #'HEAD'
            mongo_pub_version       = version.lstrip('n') if version[0] in 'n' else 'latest'
            pkg_name_suffix    = pkg_name_suffix if pkg_name_suffix else ''
            pkg_version        = pkg_version
            pkg_name_conflicts = list(self.configurator.default("pkg_name_conflicts") if pkg_name_suffix else [])
            pkg_name_conflicts.remove(pkg_name_suffix) if pkg_name_suffix and pkg_name_suffix in pkg_name_conflicts else []
            formatter          =  self.configurator.default("get_mongo") + self.configurator.default("mangle_mongo") + (self.configurator.nightly_build_mangle_files if version[0] == 'n' else '') +(self.configurator.default("build_prerequisites") if version[0] != 'n' else '') + self.configurator.default("install_for_packaging") + self.configurator.default("build_package")
            script+=self.fmt(formatter,
                             mongo_version=mongo_version,
                             distro_name=self.distro_name,
                             distro_version=self.distro_version,
                             distro_arch=self.distro_arch,
                             pkg_prereq_str=" ".join(self.pkg_prereqs),
                             pkg_name=self.pkg_name,
                             pkg_name_suffix=pkg_name_suffix,
                             pkg_version=pkg_version,
                             pkg_product_dir=self.pkg_product_dir,
                             # KLUDGE: rpm specs and deb
                             # control files use
                             # comma-separated conflicts,
                             # but there's no reason to
                             # suppose this works elsewhere
                             pkg_name_conflicts = ", ".join([self.pkg_name+conflict for conflict in pkg_name_conflicts]),
                             mongo_arch=self.mongo_arch,
                             mongo_pub_version=mongo_pub_version,
                             rpmbuild_dir=self.configurator.default('rpmbuild_dir'))
            script+='rm -rf mongo'
        return script

    def __enter__(self):
        self.localscript=None
        # One of tempfile or I is very stupid.
        (fh, name) = tempfile.mkstemp('', "makedist.", ".")
        try:
            pass
        finally:
            os.close(fh)
        with open(name, 'w+') as fh:
            fh.write(self.genscript())
        self.localscript=name
        return self
        
    def __exit__(self, type, value, traceback):
        if self.localscript:
            os.unlink(self.localscript)

class Configurator(SshConnectionConfigurator, EC2InstanceConfigurator, ScriptFileConfigurator, BaseHostConfigurator):
    def __init__(self, **kwargs):
        super(Configurator, self).__init__(**kwargs)

class rackspaceInstance(nodeWrapper):
    def __init__(self, configurator, **kwargs):
        super(rackspaceInstance, self).__init__(configurator, **kwargs)
        self.imgname=configurator.default('rackspace_imgname')

    def start(self):
        driver = get_driver(Provider.RACKSPACE)
        self.conn = driver(settings.rackspace_account, settings.rackspace_api_key)
        name=self.imgname+'-'+str(os.getpid())
        images=filter(lambda x: (x.name.find(self.imgname) > -1), self.conn.list_images())
        sizes=self.conn.list_sizes()
        sizes.sort(cmp=lambda x,y: int(x.ram)<int(y.ram))
        node = None
        if len(images) > 1:
            raise Exception("too many images with \"%s\" in the name" % self.imgname)
        if len(images) < 1:
            raise Exception("too few images with \"%s\" in the name" % self.imgname)
        image = images[0]
        self.node = self.conn.create_node(image=image, name=name, size=sizes[0])
        # Note: the password is available only in the response to the
        # create_node request, not in subsequent list_nodes()
        # requests; so although the node objects we get back from
        # list_nodes() are usuable for most things, we must hold onto
        # the initial password.
        self.password = self.node.extra['password']
        print self.node

    def list_nodes(self):
        return self.conn.list_nodes()
        
    def setup(self):
        self.putSshKey()

    def putSshKey(self):
        keyfile=settings.makedist['ssh_keyfile']
        ssh = ParamikoSSHClient(hostname = self.node.public_ip[0], password = self.password)
        ssh.connect()
        print "putting ssh public key"
        ssh.put(".ssh/authorized_keys", contents=open(keyfile+'.pub').read(), chmod=0600)
        print "ok"

def parse_mongo_version_spec (spec):
    foo = spec.split(":")
    mongo_version = foo[0] # this can be a commit id, a
                           # release id "r1.2.2", or a branch name
                           # starting with v.
    if len(foo) > 1:
        pkg_name_suffix = foo[1] 
    if len(foo) > 2 and foo[2]:
        pkg_version = foo[2]
    else:
        pkg_version = time.strftime("%Y%m%d")
    if not pkg_name_suffix:
        if mongo_version[0] in ["r", "v"]:
            nums = mongo_version.split(".")
            if int(nums[1]) % 2 == 0:
                pkg_name_suffix = "-stable"
            else:
                pkg_name_suffix = "-unstable"
        else:
            pkg_name_suffix = ""
    return (mongo_version, pkg_name_suffix, pkg_version)

def main():
#    checkEnvironment()

    (kwargs, args) = processArguments()
    (rootdir, distro_name, distro_version, arch, mongo_version_spec) = args[:5]
    # FIXME: there are a few other characters that we can't use in
    # file names on Windows, in case this program really needs to run
    # there.
    distro_name = distro_name.replace('/', '-').replace('\\', '-')
    distro_version = distro_version.replace('/', '-').replace('\\', '-')
    arch = arch.replace('/', '-').replace('\\', '-')     
    try:
        import settings
        if "makedist" in dir ( settings ):
            for key in ["ec2_sshkey", "ssh_keyfile", "gpg_homedir" ]:
                if key not in kwargs and key in settings.makedist:
                    kwargs[key] = settings.makedist[key]
    except Exception, err:
        print "No settings: %s.  Continuing anyway..." % err
        pass

    kwargs["distro_name"]    = distro_name
    kwargs["distro_version"] = distro_version
    kwargs["arch"]           = arch
    kwargs['mongo_version_spec'] = mongo_version_spec
    
    kwargs["localdir"] = rootdir
    # FIXME: this should also include the mongo version or something.
#    if "subdirs" in kwargs:
#        kwargs["localdir"] = "%s/%s/%s/%s/%s" % (rootdir, distro_name, distro_version, arch, kwargs["mongo_version"])
#    else:




    kwargs['gpg_homedir'] = kwargs["gpg_homedir"] if "gpg_homedir" in kwargs else os.path.expanduser("~/.gnupg") 
    configurator = Configurator(**kwargs)
    LocalHost.runLocally(["mkdir", "-p", kwargs["localdir"]])
    with ScriptFile(configurator, **kwargs) as script:
        with open(script.localscript) as f:
            print """# Going to run the following on a fresh AMI:"""
            print f.read()
            time.sleep(10)
        # FIXME: it's not the best to have two different pathways for
        # the different hosting services, but...
        with EC2Instance(configurator, **kwargs) if kwargs['distro_name'] != 'fedora' else rackspaceInstance(configurator, **kwargs) as host:
            host.initwait()
            host.setup()
            kwargs["ssh_host"] = host.getHostname()
            with SshConnection(configurator, **kwargs) as ssh:
                ssh.runRemotely(["uname -a; ls /"])
                ssh.runRemotely(["mkdir", "pkg"])
                if "local_mongo_dir" in kwargs:
                    ssh.sendFiles([(kwargs["local_mongo_dir"]+'/'+d, "pkg") for d in ["rpm", "debian"]])
                ssh.sendFiles([(kwargs['gpg_homedir'], ".gnupg")])
                ssh.sendFiles([(script.localscript, "makedist.sh")])
                ssh.runRemotely((["sudo"] if ssh.ssh_login != "root" else [])+ ["sh", "makedist.sh"])
                ssh.recvFiles([(script.pkg_product_dir, kwargs['localdir'])])

def processArguments():
    # flagspec [ (short, long, argument?, description, argname)* ]
    flagspec = [ ("?", "usage", False, "Print a (useless) usage message", None),
                 ("h", "help", False, "Print a help message and exit", None),
                 ("N", "no-terminate", False, "Leave the EC2 instance running at the end of the job", None),
                 ("S", "subdirs", False, "Create subdirectories of the output directory based on distro name, version, and architecture", None),
                 ("I", "use-internal-name", False, "Use the EC2 internal hostname for sshing", None),
                 (None, "gpg-homedir", True, "Local directory of gpg junk", "STRING"),
                 (None, "local-mongo-dir", True, "Copy packaging files from local mongo checkout", "DIRECTORY"),
                 ]
    shortopts = "".join([t[0] + (":" if t[2] else "") for t in flagspec if t[0] is not None])
    longopts = [t[1] + ("=" if t[2] else "") for t in flagspec]

    try:
        opts, args = getopt.getopt(sys.argv[1:], shortopts, longopts)
    except getopt.GetoptError, err:
        print str(err)
        sys.exit(2)

    # Normalize the getopt-parsed options.
    kwargs = {}
    for (opt, arg) in opts:
        flag = opt
        opt = opt.lstrip("-")
        if flag[:2] == '--': #long opt
            kwargs[opt.replace('-', '_')] = arg
        elif flag[:1] == "-": #short opt 
            ok = False
            for tuple in flagspec:
                if tuple[0] == opt:
                    ok = True
                    kwargs[tuple[1].replace('-', '_')] = arg
                    break
            if not ok:
                raise SimpleError("this shouldn't happen: unrecognized option flag: %s", opt)
        else:
            raise SimpleError("this shouldn't happen: non-option returned from getopt()")
        
    if "help" in kwargs:
        print "Usage: %s [OPTIONS] DIRECTORY DISTRO DISTRO-VERSION ARCHITECTURE MONGO-VERSION-SPEC" % sys.argv[0]
        print """Build some packages on new EC2 AMI instances, leave packages under DIRECTORY.

MONGO-VERSION-SPEC has the syntax
Commit(:Pkg-Name-Suffix(:Pkg-Version)).  If Commit starts with an 'r',
build from a tagged release; if Commit starts with an 'n', package up
a nightly build; if Commit starts with a 'v', build from the HEAD of a
version branch; otherwise, build whatever git commit is identified by
Commit.  Pkg-Name-Suffix gets appended to the package name, and
defaults to "-stable" and "-unstable" if Commit looks like it
designates a stable or unstable release/branch, respectively.
Pkg-Version is used as the package version, and defaults to YYYYMMDD.
Examples:

  HEAD             # build a snapshot of HEAD, name the package
                   # "mongodb", use YYYYMMDD for the version

  HEAD:-snap     # build a snapshot of HEAD, name the package
                 # "mongodb-snap", use YYYYMMDD for the version

  HEAD:-snap:123     # build a snapshot of HEAD, name the package
                     # "mongodb-snap", use 123 for the version

  HEAD:-suffix:1.3 # build a snapshot of HEAD, name the package
                   # "mongodb-snapshot", use "1.3 for the version

  r1.2.3           # build a package of the 1.2.3 release, call it "mongodb-stable",
                   # make the package version YYYYMMDD.

  v1.2:-stable:    # build a package of the HEAD of the 1.2 branch

  decafbad:-foo:123 # build git commit "decafbad", call the package
                    # "mongodb-foo" with package version 123.

Options:"""
        for t in flagspec:
            print "%-20s\t%s." % ("%4s--%s%s:" % ("-%s, " % t[0] if t[0] else "", t[1], ("="+t[4]) if t[4] else ""), t[3])
        print """
Mandatory arguments to long options are also mandatory for short
options."""
        sys.exit(0)

    if "usage" in kwargs:
        print "Usage: %s [OPTIONS] OUTPUT-DIR DISTRO-NAME DISTRO-VERSION ARCHITECTURE MONGO-VERSION-SPEC" % sys.argv[0]
        sys.exit(0)


    return (kwargs, args)


if __name__ == "__main__":
    main()

# Examples:

# ./makedist.py /tmp/ubuntu ubuntu 8.10 x86_64 HEAD:-snapshot,v1.4:-stable,v1.5:-unstable
# ./makedist.py /tmp/ubuntu ubuntu 8.10 x86_64 nlatest:-snapshot,n1.4.2:-stable,n1.5.0:-unstable
