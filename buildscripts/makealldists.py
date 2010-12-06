#!/usr/bin/env python

from __future__ import with_statement
import subprocess
import sys
import os
import time
import tempfile
import errno
import glob
import shutil
import settings
import simples3

def s3bucket():
    return simples3.S3Bucket(settings.bucket, settings.id, settings.key)

def s3cp (bucket, filename, s3name):
    defaultacl="public-read"
    print "putting %s to %s" % (filename, s3name)
    bucket.put(s3name, open(filename, "rb").read(), acl=defaultacl)

def pushrepo(repodir):
    files=subprocess.Popen(['find', repodir, '-type', 'f'], stdout=subprocess.PIPE).communicate()[0][:-1].split('\n')
    bucket=s3bucket()
    olddebs=[t[0] for t in bucket.listdir(prefix='distros/') if t[0].endswith('.deb')]
    newdebs=[]
    for fn in files:
        if len(fn) == 0:
            continue
        tail = fn[len(repodir):]
        # Note: be very careful not to produce s3names containing
        # sequences of repeated slashes: s3 doesn't treat a////b as
        # equivalent to a/b.
        s3name1='distros-archive/'+time.strftime('%Y%m%d')+tail
        s3name2='distros'+tail
        s3cp(bucket, fn, s3name1)
        s3cp(bucket, fn, s3name2)        
        if s3name1.endswith('.deb'):
            newdebs.append(s3name1)
    # FIXME: we ought to clean out old debs eventually, but this will
    # blow away too much if we're trying to push a subset of what's
    # supposed to be available.
    #[bucket.delete(deb) for deb in set(olddebs).difference(set(newdebs))]
    
def cat (inh, outh):
    inh.seek(0)
    for line in inh:
        outh.write(line)
    inh.close()

# This generates all tuples from mixed-radix counting system, essentially.
def gen(listlist):
    dim=len(listlist)
    a=[0 for ignore in listlist]
    while True:
        yield [listlist[i][a[i]] for i in range(dim)]
        a[0]+=1
        for j in range(dim):
            if a[j] == len(listlist[j]):
                if j<dim-1:
                    a[j+1]+=1
                else:
                    return
                a[j]=0

def dirify(string):
    return (string if string[-1:] in '\/' else string+'/')
def fileify(string):
    return (string if string[-1:] not in '\/' else string.rstrip('\/'))

# WTF: os.makedirs errors if the leaf exists?
def makedirs(f):
    try:
        os.makedirs(f)
    except OSError: # as exc: # Python >2.5
        exc=sys.exc_value
        if exc.errno == errno.EEXIST:
            pass
        else: 
            raise exc



# This is a fairly peculiar thing to want to do, but our build process
# creates several apt repositories for each mongo version we build on
# any given Debian/Ubutnu release.  To merge repositories together, we
# must concatenate the Packages.gz files.
def merge_directories_concatenating_conflicts (target, sources):
    print sources
    target = dirify(target)
    for source in sources:
        source = dirify(source)
        files = subprocess.Popen(["find", source, "-type", "f"], stdout=subprocess.PIPE).communicate()[0].split('\n')
        for f in files:
            if f == '':
                continue
            rel = f[len(source):]
            o=target+rel
            makedirs(os.path.dirname(o))
            with open(f) as inh:
                with open(target+rel, "a") as outh:
                    outh.write(inh.read())


def parse_mongo_version_spec(spec):
    l = spec.split(':')
    if len(l) == 1:
        l+=['','']
    elif len(l) == 2:
        l+=['']
    return l

def logfh(distro, distro_version, arch):
    prefix = "%s-%s-%s.log." % (distro, distro_version, arch)
    # This is a NamedTemporaryFile mostly so that I can tail(1) them
    # as we go.
    return tempfile.NamedTemporaryFile("w+b", -1, prefix=prefix)

def spawn(distro, distro_version, arch, spec, directory, opts):
    argv = ["python", "makedist.py"] + opts + [ directory, distro, distro_version, arch ] + [ spec ]
#    cmd = "mkdir -p %s; cd %s; touch foo.deb; echo %s %s %s %s %s | tee Packages " % ( directory, directory, directory, distro, distro_version, arch, mongo_version )
#    print cmd
#    argv = ["sh", "-c", cmd]
    fh = logfh(distro, distro_version, arch)
    print >> fh, "Running %s" % argv
    # it's often handy to be able to run these things at the shell
    # manually.  FIXME: this ought to be slightly less than thoroughly
    # ignorant of quoting issues (as is is now).
    print >> fh, " ".join(argv)
    fh.flush()
    proc = subprocess.Popen(argv, stdin=None, stdout=fh, stderr=fh)        
    return (proc, fh, distro, distro_version, arch, spec)

def win(name, logfh, winfh):
    logfh.seek(0)
    print >> winfh, "=== Winner %s ===" % name
    cat(logfh, winfh)
    print >> winfh, "=== End winner %s ===" % name

def lose(name, logfh, losefh):
    logfh.seek(0)
    print >> losefh, "=== Loser %s ===" % name
    cat(logfh, losefh)
    print >> losefh, "=== End loser %s ===" % name

def wait(procs, winfh, losefh, winners, losers):
    print "."
    sys.stdout.flush()
    try:
        (pid, stat) = os.wait()
    except OSError, err:
        print >> sys.stderr, "This shouldn't happen."
        print >> sys.stderr, err
        next
    if pid:
        [tup] = [tup for tup in procs if tup[0].pid == pid]
        (proc, logfh, distro, distro_version, arch, spec) = tup
        procs.remove(tup)
        name = "%s %s %s" % (distro, distro_version, arch)
        if os.WIFEXITED(stat):
            if os.WEXITSTATUS(stat) == 0:
                win(name, logfh, winfh)
                winners.append(name)
            else:
                lose(name, logfh, losefh)
                losers.append(name)
        if os.WIFSIGNALED(stat):
            lose(name, logfh, losefh)
            losers.append(name)



def __main__():
    # FIXME: getopt & --help.
    print  " ".join(sys.argv)
    branches = sys.argv[-1]
    makedistopts = sys.argv[1:-1]

    # Output from makedist.py goes here.
    outputroot=tempfile.mkdtemp()
    repodir=tempfile.mkdtemp()

    print "makedist output under: %s\ncombined repo: %s\n" % (outputroot, repodir)
    sys.stdout.flush()
    # Add more dist/version/architecture tuples as they're supported.
    dists = (("ubuntu", "10.10"),
             ("ubuntu", "10.4"),
             ("ubuntu", "9.10"),
             ("ubuntu", "9.4"),
             ("ubuntu", "8.10"),
             ("debian", "5.0"),
             ("centos", "5.4"),
             ("fedora", "12"),
             ("fedora", "13"),
             ("fedora", "14"))
    arches = ("x86", "x86_64")
#    mongos = branches.split(',')
    # Run a makedist for each distro/version/architecture tuple above.
    winners = []
    losers = []
    winfh=tempfile.TemporaryFile()
    losefh=tempfile.TemporaryFile()
    procs = []
    count = 0
    for ((distro, distro_version), arch, spec) in gen([dists, arches, [branches]]):
        # FIXME: now x86 fedoras on RackSpace circa 04/10.
        if distro == "fedora" and arch == "x86":
            continue
        count+=1
        opts = makedistopts
        if distro in ["debian", "ubuntu"]:
            outputdir = "%s/deb/%s" % (outputroot, distro)
        elif distro in ["centos", "fedora", "redhat"]:
            outputdir = "%s/rpm/%s/%s/os" % (outputroot, distro, distro_version)
        else:
            raise Exception("unsupported distro %s" % distro)
            #opts += ["--subdirs"]

        procs.append(spawn(distro, distro_version, arch, spec, outputdir, opts))

        if len(procs) == 8:
            wait(procs, winfh, losefh, winners, losers)

    while procs:
        wait(procs, winfh, losefh, winners, losers)

    winfh.seek(0)
    losefh.seek(0)
    nwinners=len(winners)
    nlosers=len(losers)
    print "%d winners; %d losers" % (nwinners, nlosers)
    cat(winfh, sys.stdout)
    cat(losefh, sys.stdout)
    print "%d winners; %d losers" % (nwinners, nlosers)
    if count == nwinners + nlosers:
        print "All jobs accounted for"
#        return 0
    else:
        print "Lost some jobs...?"
        return 1

    sys.stdout.flush()
    sys.stderr.flush()
    
    # this is sort of ridiculous, but the outputs from rpmbuild look
    # like RPM/<arch>, but the repo wants to look like
    # <arch>/RPM.
    for dist in os.listdir(outputroot+'/rpm'):
        if dist in ["centos", "fedora", "redhat"]:
            distdir="%s/rpm/%s" % (outputroot, dist)
            rpmdirs = subprocess.Popen(["find", distdir, "-type", "d", "-a", "-name", "RPMS"], stdout=subprocess.PIPE).communicate()[0].split('\n')[:-1]
            for rpmdir in rpmdirs:
                for arch in os.listdir(rpmdir):
                    archdir="%s/../%s" % (rpmdir, arch)
                    os.mkdir(archdir)
                    os.rename("%s/%s" % (rpmdir, arch), "%s/RPMS" % (archdir,))
                os.rmdir(rpmdir)


    for flavor in os.listdir(outputroot):
        argv=["python", "mergerepositories.py", flavor, "%s/%s" % (outputroot, flavor), repodir]
        print "running %s" % argv
        print " ".join(argv)
        r = subprocess.Popen(argv).wait()
        if r != 0:
            raise Exception("mergerepositories.py exited %d" % r)
        print repodir
    #pushrepo(repodir)
    #shutil.rmtree(outputroot)
    #shutil.rmtree(repodir)

    return 0


if __name__ == '__main__':
    __main__()


# FIXME: this ought to be someplace else.

# FIXME: remove this comment when the buildbot does this.  After this
# program, run something that amounts to
#
#  find /tmp/distros -name *.deb -or -name Packages.gz | while read f; do echo "./s3cp.py $f ${f#/tmp/}"; done
#
# where ./s3cp.py is a trivial s3 put executable in this directory.

# merge_directories_concatenating_conflicts('/tmp/distros/debian', '/tmp/distros-20100222/debian/HEAD', '/tmp/distros-20100222/debian/r1.3.2','/tmp/distros-20100222/debian/v1.2')

# merge_directories_concatenating_conflicts('/tmp/distros/ubuntu', '/tmp/distros-20100222/ubuntu/HEAD', 	'/tmp/distros-20100222/ubuntu/r1.3.2', '/tmp/distros-20100222/ubuntu/v1.2')
