#!/usr/bin/env python

# makedist.py: invoke EC2 AMI instances and run distribution packaging
# programs for Unixy hosts.

# For ease of use, put a file called settings.py someplace in your
# sys.path, containing something like the following:

# makedist = {
#     # ec2-api-tools needs the following two set in the process
#     # environment.
#     "EC2_HOME": "/path/to/ec2-api-tools",
#     # The EC2 tools won't run at all unless this variable is set to a directory
#     # relative to which a "bin/java" exists.
#     "JAVA_HOME" : "/usr",
#     # All the ec2-api-tools take these two as arguments.
#     # Alternatively, you can set the environment variables EC2_PRIVATE_KEY and EC2_CERT
#     # respectively, leave these two out of settings.py, and let the ec2 tools default.
#     "pkey": "/path/to/pk-file.pem"
#     "cert" : "/path/to/cert-file.pem"
#     # This gets supplied to ec2-run-instances to rig up an ssh key for
#     # the remote user.
#     "sshkey" : "key-id",
#     # And so we need to tell our ssh processes where to find the
#     # appropriate public key file.
#     "sshkeyfile" : "/path/to/key-id-file"
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
import getopt
import socket
import time
import os.path
import tempfile

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

    
# For mistakes implementing this program.
class Bug(SimpleError):
    def __init__(self, *args):
        super(Bug, self).__init__(args)

class Configurator (object):
    # This is my attempt to factor as much as possible into
    # non-repeated fragments, to keep program logic and hierarchy
    # sane.  The idea is to default as much as possible just from the
    # user's specification of the distro, version, architecture, and
    # mongo version to use.

    # Note: we need to insinuate a couple things into the shell
    # fragment we run on the remote host, so we'll do it with shell
    # environment variables.
    preamble_commands = """
set -x # verbose execution, for debugging
set -e # errexit, stop on errors
set -u # nounset, treat unset shell vars as errors
export PREREQS="%s"
export MONGO_NAME="%s"
export MONGO_VERSION="%s"
export MONGO_URL="%s"
export MODE="%s"
export ARCH="%s"
export PKG_VERSION="%s"
export DISTRO_VERSION="%s"
export PRODUCT_DIR="%s"
# the following are derived from the preceding
export MONGO_WORKDIR="$MONGO_NAME-$PKG_VERSION"
"""
    get_mongo_commands = """
case "$MODE" in
  "release")
    # TODO: some Unixes don't ship with wget.  Add support other http clients.
    # Or just use git for this...
    wget -Otarball.tgz "$MONGO_URL"
    ;;
  "commit")
    git clone git://github.com/mongodb/mongo.git
    ( cd mongo && git archive --prefix=mongo-git-archive/ "$MONGO_VERSION" ) | gzip > tarball.tgz
    ;;
  *)
    echo "bad mode $MODE"; exit 1;;
esac
# Ensure that there's just one toplevel directory in the tarball.
test `tar tzf tarball.tgz | sed 's|/.*||' | sort -u | wc -l` -eq 1
tar xzf tarball.tgz
mv "`tar tzf tarball.tgz | sed 's|/.*||' | sort -u | head -n1`" "$MONGO_WORKDIR"
if [ "$MODE" = "commit" ]; then
  ( cd "$MONGO_WORKDIR" && python ./buildscripts/frob_version.py "$PKG_VERSION" ) || exit 1
fi
"""
    deb_prereq_commands = """
# Configure debconf to never prompt us for input.
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y $PREREQS
"""
    deb_build_commands="""
mkdir -p "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/binary-$ARCH
mkdir -p "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/source
( cd "$MONGO_WORKDIR"; debuild ) || exit 1
mv mongodb_*.deb "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/binary-$ARCH
mv mongodb_*.dsc "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/source
mv mongodb_*.tar.gz "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/source
( dpkg-scanpackages "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/binary-$ARCH /dev/null | gzip -9c > "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/binary-$ARCH/Packages.gz;
dpkg-scansources "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/source /dev/null | gzip -9c > "$PRODUCT_DIR"/"$DISTRO_VERSION"/10gen/source/Sources.gz )
"""
    rpm_prereq_commands = """
rpm -Uvh http://download.fedora.redhat.com/pub/epel/5/x86_64/epel-release-5-3.noarch.rpm
yum -y install $PREREQS
RPMBUILDROOT=/usr/src/redhat
for d in BUILD  BUILDROOT  RPMS  SOURCES  SPECS  SRPMS; do mkdir -p "$RPMBUILDROOT"/$d; done
cp -v "$MONGO_WORKDIR"/rpm/mongo.spec "$RPMBUILDROOT"/SPECS
tar -cpzf "$RPMBUILDROOT"/SOURCES/"$MONGO_WORKDIR".tar.gz "$MONGO_WORKDIR"
"""
    rpm_build_commands="""
rpmbuild -ba /usr/src/redhat/SPECS/mongo.spec
""" 
    # FIXME: this is clean, but adds 40 minutes or so to the build process.
    old_rpm_precommands = """
yum install -y bzip2-devel python-devel libicu-devel chrpath zlib-devel nspr-devel readline-devel ncurses-devel
wget ftp://194.199.20.114/linux/EPEL/5Client/SRPMS/js-1.70-8.el5.src.rpm
rpm -ivh js-1.70-8.el5.src.rpm
sed -i 's/XCFLAGS.*$/XCFLAGS=\"%{optflags} -fPIC -DJS_C_STRINGS_ARE_UTF8\" \\\\/' /usr/src/redhat/SPECS/js.spec
rpmbuild -ba /usr/src/redhat/SPECS/js.spec
rpm -Uvh /usr/src/redhat/RPMS/$ARCH/js-1.70-8.x86_64.rpm
rpm -Uvh /usr/src/redhat/RPMS/$ARCH/js-devel-1.70-8.x86_64.rpm
wget ftp://195.220.108.108/linux/sourceforge/g/project/gr/gridiron2/support-files/FC10%20source%20RPMs/boost-1.38.0-1.fc10.src.rpm
rpm -ivh boost-1.38.0-1.fc10.src.rpm
rpmbuild -ba /usr/src/redhat/SPECS/boost.spec
rpm -ivh /usr/src/redhat/RPMS/$ARCH/boost-1.38.0-1.x86_64.rpm
rpm -ivh /usr/src/redhat/RPMS/$ARCH/boost-devel-1.38.0-1.x86_64.rpm
"""

    old_deb_boost_prereqs =  ["libboost-thread1.35-dev", "libboost-filesystem1.35-dev", "libboost-program-options1.35-dev", "libboost-date-time1.35-dev", "libboost1.35-dev"]
    new_deb_boost_prereqs = [ "libboost-thread-dev", "libboost-filesystem-dev", "libboost-program-options-dev", "libboost-date-time-dev", "libboost-dev" ]
    common_deb_prereqs = [ "build-essential", "dpkg-dev", "libreadline-dev", "libpcap-dev", "libpcre3-dev", "xulrunner-dev", "git-core", "scons", "debhelper", "devscripts", "git-core" ]

    centos_preqres = ["js-devel", "readline-devel", "pcre-devel", "gcc-c++", "scons", "rpm-build", "git" ]
    fedora_prereqs = ["js-devel", "readline-devel", "pcre-devel", "gcc-c++", "scons", "rpm-build", "git" ]

    deb_productdir = "dists"
    rpm_productdir = "/usr/src/redhat" # FIXME: is this correct?

    # So here's a tree of data for the details that differ among
    # platforms.  If you change the structure, remember to also change
    # the definition of lookup() below.
    configuration = (("ami",
                      ((("ubuntu", "10.4", "x86_64"), "ami-818965e8"),
                       (("ubuntu", "10.4", "x86"), "ami-ef8d6186"),
                       (("ubuntu", "9.10", "x86_64"), "ami-55739e3c"),
                       (("ubuntu", "9.10", "x86"), "ami-bb709dd2"),
                       (("ubuntu", "9.4", "x86_64"), "ami-eef61587"),
                       (("ubuntu", "9.4", "x86"), "ami-ccf615a5"),
                       (("ubuntu", "8.10", "x86"), "ami-c0f615a9"),
                       (("ubuntu", "8.10", "x86_64"), "ami-e2f6158b"),
                       # Doesn't work: boost too old.
                       #(("ubuntu", "8.4", "x86"), "ami59b35f30"),
                       #(("ubuntu", "8.4", "x86_64"), "ami-27b35f4e"),
                       (("debian", "5.0", "x86"), "ami-dcf615b5"),
                       (("debian", "5.0", "x86_64"), "ami-f0f61599"),
                       (("centos", "5.4", "x86"), "ami-f8b35e91"),
                       (("centos", "5.4", "x86_64"), "ami-ccb35ea5"),
                       (("fedora", "8", "x86_64"), "ami-2547a34c"),
                       (("fedora", "8", "x86"), "ami-5647a33f"))),
                     ("mtype",
                      ((("*", "*", "x86"), "m1.small"),
                       (("*", "*", "x86_64"), "m1.large"))),
                     ("productdir",
                      ((("ubuntu", "*", "*"), deb_productdir),
                       (("debian", "*", "*"), deb_productdir),
                       (("fedora", "*", "*"), rpm_productdir),
                       (("centos", "*", "*"), rpm_productdir))),
                     ("prereqs",
                      ((("ubuntu", "9.4", "*"), old_deb_boost_prereqs + common_deb_prereqs),
                       (("ubuntu", "9.10", "*"), new_deb_boost_prereqs + common_deb_prereqs),
                       (("ubuntu", "10.4", "*"), new_deb_boost_prereqs + common_deb_prereqs),
                       (("ubuntu", "8.10", "*"), old_deb_boost_prereqs + common_deb_prereqs),
                       # Doesn't work: boost too old.
                       #(("ubuntu", "8.4", "*"), old_deb_boost_prereqs + common_deb_prereqs),
                       (("debian", "5.0", "*"), old_deb_boost_prereqs + common_deb_prereqs),
                       (("fedora", "8", "*"), fedora_prereqs),
                       (("centos", "5.4", "*"), centos_preqres))),
                     ("preamble_commands",
                      ((("*", "*", "*"), preamble_commands),)),
                     ("commands",
                      ((("debian", "*", "*"), deb_prereq_commands + get_mongo_commands + deb_build_commands),
                       (("ubuntu", "*", "*"), deb_prereq_commands + get_mongo_commands + deb_build_commands),
                       (("centos", "*", "*"), old_rpm_precommands + rpm_prereq_commands + get_mongo_commands + rpm_build_commands),
                       (("fedora", "*", "*"), old_rpm_precommands + rpm_prereq_commands + get_mongo_commands + rpm_build_commands))),
                     ("login",
                      # ...er... this might rather depend on the ami
                      # than the distro tuple...
                      ((("debian", "*", "*"), "root"),
                       (("ubuntu", "10.4", "*"), "ubuntu"),
                       (("ubuntu", "9.10", "*"), "ubuntu"),
                       (("ubuntu", "9.4", "*"), "root"),
                       (("ubuntu", "8.10", "*"), "root"),
                       # Doesn't work: boost too old.
                       #(("ubuntu", "8.4", "*"), "ubuntu"),
                       (("centos", "*", "*"), "root"))),
                     # What do they call this architecture on this architecture...?
                     ("distarch",
                      ((("debian", "*", "x86_64"), "amd64"),
                       (("ubuntu", "*", "x86_64"), "amd64"),
                       (("debian", "*", "x86"), "i386"),
                       (("ubuntu", "*", "x86"), "i386"),
                       (("*", "*", "x86_64"), "x86_64"),
                       (("*", "*", "x86"), "x86"))),
                     # This is ridiculous, but it's already the case
                     # that different distributions have different names for the package.
                     ("mongoname",
                      ((("debian", "*", "*"), "mongodb"),
                       (("ubuntu", "*", "*"), "mongodb"),
                       (("centos", "*", "*"), "mongo"),
                       (("fedora", "*", "*"), "mongo"))))

    def lookup(cls, what, dist, vers, arch):
        for (wht, seq) in cls.configuration:
            if what == wht:
                for ((dpat, vpat, apat), payload) in seq:
                    # For the moment, our pattern facility is just "*" or exact match.
                    if ((dist == dpat or dpat == "*") and
                        (vers == vpat or vpat == "*") and
                        (arch == apat or apat == "*")):
                        return payload
        raise SimpleError("couldn't find a%s %s configuration for dist=%s, version=%s, arch=%s",
                          "n" if ("aeiouAEIOU".find(what[0]) > -1) else "",
                          what, dist, vers, arch)

    # Public interface.  Yeah, it's stupid, but I'm in a hurry.
    @classmethod
    def default(cls, what, distro, version, arch):
        return cls().lookup(what, distro, version, arch)
    @classmethod
    def findOrDefault(cls, dict, what, distro, version, arch):
        """Lookup the second argument in the first argument, or else
employ the Configurator's defaulting with the last 3 arguments."""
        return (dict[what] if what in dict else cls().lookup(what, distro, version, arch))

class BaseBuilder(object):
    """A base class.  Represents a host (in principle possibly the
    localhost, though that's not implemented at the moment)."""
    def __init__(self, **kwargs):
        # Don't call super's __init__(): it doesn't take arguments.
        # Instance variables that several of our subclasses will want.
        self.distro=kwargs["distro"]
        self.version=kwargs["version"]
        self.arch=kwargs["arch"]
        self.distarch=self.default("distarch")
        self.mode="commit" if "commit" in kwargs else "release"
        self.mongoversion=(kwargs["mongoversion"][:6] if "commit" in kwargs else kwargs["mongoversion"])

        self.mongoname=self.default("mongoname")
        self.mongourl=(("http://download.github.com/mongodb-mongo-%s.tar.gz" % self.mongoversion) if "commit" in kwargs else ("http://github.com/mongodb/mongo/tarball/r"+self.mongoversion))

        self.localdir=kwargs["localdir"]

        # FIXME: for the moment, gpg signing is disabled. Either get
        # rid of this stuff, or make gpg signing work, either on the
        # remote host or locally.
        self.localgpgdir = kwargs["localgpgdir"] if "localgpgdir" in kwargs else os.path.expanduser("~/.gnupg")
        self.remotegpgdir = kwargs["remotegpgdir"]  if "remotegpgdir" in kwargs else ".gnupg"

        if "localscript" in kwargs:
            self.localscript = kwargs["localscript"]
        else:
            self.localscript = None
        self.remotescript = kwargs["remotescript"] if "remotescript" in kwargs else "makedist.sh"

        self.prereqs=self.default("prereqs")
        self.productdir=self.default("productdir")
        self.pkgversion = kwargs["pkgversion"] if "pkgversion" in kwargs else time.strftime("%Y%m%d")
        self.commands=(self.default("preamble_commands") % (" ".join(self.prereqs), self.mongoname, self.mongoversion, self.mongourl, self.mode, self.distarch, self.pkgversion, self.version, self.productdir)) + self.default("commands")


    def default(self, what):
        return Configurator.default(what, self.distro, self.version, self.arch)
    def findOrDefault(self, kwargs, what):
        return Configurator.findOrDefault(kwargs, what, self.distro, self.version, self.arch)

    def runLocally(self, argv):
        print "running %s" % argv
        r = subprocess.Popen(argv).wait()
        if r != 0:
            raise SubcommandError("subcommand %s exited %d", argv, r)
        
    def do(self):
        # KLUDGE: this mkdir() shouldn't really be here, but if we're
        # going to fail due to local permissions or whatever, we ought
        # to do it before wasting tens of minutes building.
        self.runLocally(["mkdir", "-p", self.localdir])
        try:
            self.generateConfigs()
            self.sendConfigs()
            self.doBuild()
            self.recvDists()
        finally:
            self.cleanup()

    def generateConfigs(self):
        if self.localscript == None:
            fh = None
            name = None
            try:
                (fh, name) = tempfile.mkstemp('', "makedist.", ".")
                self.localscript = name
            finally:
                if fh is not None:
                    os.close(fh)
            if name is None:
                raise SimpleError("problem creating tempfile, maybe?")
        with open(self.localscript, "w") as f:
            f.write(self.commands)
    def sendConfigs(self):
        self.sendFiles([(self.localgpgdir, self.remotegpgdir)])
        self.sendFiles([(self.localscript, self.remotescript)])
    def doBuild(self):
        # TODO: abstract out the "sudo" here into a more general "run
        # a command as root" mechanism.  (Also, maybe figure out how
        # to run as much of the build as possible without root
        # privileges; we probably only need root to install
        # build-deps.).
        self.runRemotely((["sudo"] if self.login != "root" else [])+ ["sh", "-x", self.remotescript])
    def recvDists(self):
        self.recvFiles([(self.productdir,self.localdir)])
    def cleanup(self):
        os.unlink(self.localscript)

    # The rest of this class are just reminders to implement various things.
    def __enter__(self):
        Bug("instance %s doesn't implement __enter__", self)
    def __exit__(self, type, value, traceback):
        Bug("instance %s doesn't implement __exit__", self)
    def runRemotely(self, argv):
        Bug("instance %s doesn't implement runRemotely", self)
    def sendFiles(self):
        Bug("instance %s doesn't implement sendFiles", self)
    def recvFiles(self):
        Bug("instance %s doesn't implement recvFiles", self)



class EC2InstanceBuilder (BaseBuilder):
    def __init__(self, **kwargs):
        super(EC2InstanceBuilder, self).__init__(**kwargs)

        # Stuff we need to start an instance: AMI name, key and cert
        # files.  AMI and mtype default to configuration in this file,
        # but can be overridden.
        self.ami=self.findOrDefault(kwargs, "ami")
        self.mtype=self.findOrDefault(kwargs, "mtype")

        # Authentication stuff defaults according to the conventions
        # of the ec2-api-tools.
        self.cert=kwargs["cert"]
        self.pkey=kwargs["pkey"]
        self.sshkey=kwargs["sshkey"]
        self.use_internal_name = True if "use-internal-name" in kwargs else False
        self.terminate=False if "no-terminate" in kwargs else True

    def start(self):
        "Fire up a fresh EC2 instance."
        argv = ["ec2-run-instances",
                self.ami,
                "-K", self.pkey,
                "-C", self.cert,
                "-k", self.sshkey,
                "-t", self.mtype,
                "-g", "buildbot-slave", "-g", "dist-slave", "-g", "default"]
        print "running %s" % argv
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE)
        try:
            # for the moment, we only really care about the instance
            # identifier, in the second line of output:
            proc.stdout.readline() # discard line 1
            self.ident = proc.stdout.readline().split()[1]
            if self.ident == "":
                raise SimpleError("ident is empty")
            else:
                print "Instance id: %s" % self.ident
        finally:
            r = proc.wait()
            if r != 0:
                raise SimpleError("ec2-run-instances exited %d", r)

    def initdata(self):
        # poll the instance description until we get a hostname.
        # Note: it seems there can be a time interval after
        # ec2-run-instance finishes during which EC2 will tell us that
        # the instance ID doesn't exist.  This is sort of bad.
        state = "pending"
        numtries = 0
        giveup = 5
        while state == "pending":
            argv = ["ec2-describe-instances", "-K", self.pkey, "-C", self.cert, self.ident]
            proc = subprocess.Popen(argv, stdout=subprocess.PIPE)
            try:
                proc.stdout.readline() #discard line 1
                line = proc.stdout.readline()
                if line:
                    fields = line.split()
                    if len(fields) > 2:
                        state = fields[3]
                        # Sometimes, ec2 hosts die before we get to do
                        # anything with them.
                        if state == "terminated":
                            raise SimpleError("ec2 host %s died suddenly; check the aws management panel?" % self.ident)
                        if self.use_internal_name:
                            self.hostname = fields[4]
                        else:
                            self.hostname = fields[3]
                    else:
                        raise SimpleError("trouble parsing ec2-describe-instances output\n%s", line)
            finally:
                r = proc.wait()
                if r != 0 and numtries >= giveup:
                    raise SimpleError("ec2-run-instances exited %d", r)
                numtries+=1
            time.sleep(3) # arbitrary

    def stop(self):
        if self.terminate:
            self.runLocally(["ec2-terminate-instances", "-K", self.pkey, "-C", self.cert, self.ident])
        else:
            print "Not terminating EC2 instance %s." % self.ident

    def __enter__(self):
        self.start()
        self.initdata()
        return self

    def __exit__(self, type, value, traceback):
        self.stop()

# FIXME: why is this a subclass of EC2InstanceBuilder (as opposed to
# an orthogonal mixin class?)?
class SshableBuilder (BaseBuilder):
    def __init__(self, **kwargs):
        super(SshableBuilder, self).__init__(**kwargs)
        # Stuff we need to talk to the thing properly
        self.login=self.findOrDefault(kwargs, "login")

        # FIXME: use 10gen setup.py to find this.
        self.sshkeyfile=kwargs["sshkeyfile"]
        # Gets set to False when we think we can ssh in.
        self.sshwait = True

    def sshWait(self):
        "Poll until somebody's listening on port 22"
        if self.sshwait == False:
            return
        while self.sshwait:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                try:
                    s.connect((self.hostname, 22))
                    self.sshwait = False
                    print "connected on port 22 (ssh)"
                    time.sleep(15) # arbitrary timeout, in case the
                                  # remote sshd is slow.
                except socket.error, err:
                    pass
            finally:
                s.close()
                time.sleep(3) # arbitrary timeout

    def runRemotely(self, argv):
        """Run a command on the host."""
        self.sshWait()
        self.runLocally(["ssh", "-o", "StrictHostKeyChecking no",
                         "-l", self.login,
                         "-i",  self.sshkeyfile,
                         self.hostname] + argv)

    def sendFiles(self, files):
        self.sshWait()
        for (localfile, remotefile) in files:
            self.runLocally(["scp", "-o", "StrictHostKeyChecking no",
                             "-i",  self.sshkeyfile,
                             "-rv", localfile,
                             self.login + "@" + self.hostname + ":" +
                             ("" if remotefile is None else remotefile) ])

    def recvFiles(self, files):
        self.sshWait()
        print files
        for (remotefile, localfile) in files:
            self.runLocally(["scp", "-o", "StrictHostKeyChecking no",
                             "-i",  self.sshkeyfile,
                             "-rv", 
                             self.login + "@" + self.hostname +
                             ":" + remotefile,
                             "." if localfile is None else localfile ])

class Builder (EC2InstanceBuilder, SshableBuilder, BaseBuilder):
    def __init__(self, **kwargs):
        self.retries = 3 # Arbitrary.
        super(Builder,self).__init__(**kwargs)

#     # Good grief.  ssh'ing to EC2 sometimes doesn't work, but it's
#     # intermittent.  Retry everything a few times.
#     def sendFiles(self, files):
#         for i in range(self.retries):
#             try:
#                 super(Builder, self).sendFiles(files)
#                 break
#             except SubcommandError, err:
#                 if err.status == 255 and i == self.retries-1:
#                     raise err
#     def recvFiles(self, files):
#         for i in range(self.retries):
#             try:
#                 super(Builder, self).recvFiles(files)
#                 break
#             except SubcommandError, err:
#                 if err.status == 255 and i == self.retries-1:
#                     raise err
#     def runRemotely(self, argv):
#         for i in range(self.retries):
#             try:
#                 super(Builder, self).runRemotely(argv)
#                 break
#             except SubcommandError, err:
#                 if err.status == 255 and i == self.retries-1:
#                     raise err


# export EC2_HOME=/Users/kreuter/mongo/ec2-api-tools-1.3-46266 
# export PATH=$EC2_HOME/bin:$PATH
# export JAVA_HOME="/usr" # FIXME: document this braindamage
    
def main():
#    checkEnvironment()

    (kwargs, (rootdir, distro, version, arch, mongoversion)) = processArguments()
    # FIXME: there are a few other characters that we can't use in
    # file names on Windows, in case this program really needs to run
    # there.
    distro = distro.replace('/', '-').replace('\\', '-')
    version = version.replace('/', '-').replace('\\', '-')
    arch = arch.replace('/', '-').replace('\\', '-')     
    try:
        sys.path+=['.', '..', '../..']
        import settings
        if "makedist" in dir ( settings ):
            for key in ["EC2_HOME", "JAVA_HOME"]:
                if key in settings.makedist:
                    os.environ[key] = settings.makedist[key]
            for key in ["pkey", "cert", "sshkey", "sshkeyfile" ]:
                if key not in kwargs and key in settings.makedist:
                    kwargs[key] = settings.makedist[key]
    except Exception, err:
        print "No settings: %s.  Continuing anyway..." % err
        pass

    # Ensure that PATH contains $EC2_HOME/bin
    vars = ["EC2_HOME", "JAVA_HOME"]
    for var in vars:
        if os.getenv(var) == None:
            raise SimpleError("Environment variable %s is unset; did you create a settings.py?", var)

    if len([True for x in os.environ["PATH"].split(":") if x.find(os.environ["EC2_HOME"]) > -1]) == 0:
        os.environ["PATH"]=os.environ["EC2_HOME"]+"/bin:"+os.environ["PATH"]

    if "subdirs" in kwargs:
        kwargs["localdir"] = "%s/%s/%s/%s" % (rootdir, distro, version, arch)
    else:
        kwargs["localdir"] = rootdir

    kwargs["distro"] = distro
    kwargs["version"] = version
    kwargs["arch"] = arch
    kwargs["mongoversion"] = mongoversion

    with Builder(**kwargs) as host:
        print "Hostname: %s" % host.hostname
        host.do()

# def checkEnvironment():

def processArguments():
    # flagspec [ (short, long, argument?, description, argname)* ]
    flagspec = [ ("?", "usage", False, "Print a (useless) usage message", None),
                 ("h", "help", False, "Print a help message and exit", None),
                 ("N", "no-terminate", False, "Leave the EC2 instance running at the end of the job", None),
                 ("S", "subdirs", False, "Create subdirectories of the output directory based on distro name, version, and architecture.", None),
                 ("c", "commit", False, "Treat the version argument as git commit id rather than released version", None),
                 ("V", "pkgversion", True, "Use STRING as the package version number, rather than the date", "STRING"),
                 ("I", "use-internal-name", False, "Use the EC2 internal hostname for sshing", None),
                 (None, "localgpgdir", True, "Local gnupg \"homedir\" containing key for signing dist", "DIR"),

                 # These get defaulted, but the user might want to override them.
                 ("A", "ami", True, "EC2 AMI id to use", "ID"),
                 ("l", "login", True, "User account for ssh access to host", "LOGIN"),

                 

                 # These get read from settings.py, but the user might
                 # want to override them.
                 ("C", "cert", True, "EC2 X.509 certificate file", "FILE"),
                 ("F", "sshkeyfile", True, "ssh key file name", "FILE"),
                 ("K", "pkey", True, "EC2 X.509 PEM file", "FILE"),
                 ("k", "sshkey", True, "ssh key identifier (in EC2's namespace)", "STRING"),
                 ("t", "mtype", True, "EC2 machine type", "STRING") ]
    shortopts = "".join([t[0] + (":" if t[2] else "") for t in flagspec if t[0] is not None])
    longopts = [t[1] + ("=" if t[2] else "") for t in flagspec]

    try:
        opts, args = getopt.getopt(sys.argv[1:], shortopts, longopts)
    except getopt.GetoptError, err:
        print str(err)
        return 2

    # Normalize the getopt-parsed options.
    kwargs = {}
    for (opt, arg) in opts:
        flag = opt
        opt = opt.lstrip("-")
        if flag[:2] == '--': #long opt
            kwargs[opt] = arg
        elif flag[:1] == "-": #short opt 
            ok = False
            for tuple in flagspec:
                if tuple[0] == opt:
                    ok = True
                    kwargs[tuple[1]] = arg
                    break
            if not ok:
                raise SimpleError("this shouldn't happen: unrecognized option flag: %s", opt)
        else:
            raise SimpleError("this shouldn't happen: non-option returned from getopt()")
        
    if "help" in kwargs:
        print "Usage: %s [OPTIONS] DIRECTORY DISTRO DISTRO-VERSION ARCHITECTURE MONGO-VERSION" % sys.argv[0]
        print """Build some packages on new EC2 AMI instances, leave packages under DIRECTORY.
Options:"""
        for t in flagspec:
            print "%-20s\t%s." % ("%4s--%s%s:" % ("-%s, " % t[0] if t[0] else "", t[1], ("="+t[4]) if t[4] else ""), t[3])
        print """
Mandatory arguments to long options are also mandatory for short
options.  Some EC2 arguments default to (and override) environment
variables; see the ec2-api-tools documentation."""
        sys.exit(0)

    if "usage" in kwargs:
        print "Usage: %s [OPTIONS] distro distro-version architecture mongo-version" % sys.argv[0]
        sys.exit(0)


    return (kwargs, args)

if __name__ == "__main__":
    main()

# Examples:

# time ./makedist.py ubuntu 9.10 x86_64 1.3.1

# Specifying various full paths:

# time ./makedist.py -K /Users/kreuter/mongo/ops/ec2/noc-admin/pk-US7YBXUQ3YJGC5YATLXOCZY2IBTAN6A3.pem  -C /Users/kreuter/mongo/ops/ec2/noc-admin/cert-US7YBXUQ3YJGC5YATLXOCZY2IBTAN6A3.pem -k kp1 -F /Users/kreuter/mongo/ops/ec2/noc-admin/id_rsa-kp1 ubuntu 9.10 x86 1.3.1

# time ./makedist.py -K /Users/kreuter/mongo/ops/ec2/noc-admin/pk-US7YBXUQ3YJGC5YATLXOCZY2IBTAN6A3.pem  -C /Users/kreuter/mongo/ops/ec2/noc-admin/cert-US7YBXUQ3YJGC5YATLXOCZY2IBTAN6A3.pem -k kp1 -F /Users/kreuter/mongo/ops/ec2/noc-admin/id_rsa-kp1 ubuntu 9.10 x86_64 1.3.1
