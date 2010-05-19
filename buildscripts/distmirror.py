#!/usr/bin/python

# Download mongodb stuff (at present builds, sources, docs, but not
# drivers).

# Usage: <progname> [directory] # directory defaults to cwd.

# FIXME: this script is fairly sloppy.
import sys
import os
import urllib2
import time
import hashlib
import warnings

def report(url, filename):
    print "downloading %s to %s" % (url, filename)    

def checkmd5(md5str, filename):
    m = hashlib.md5()
    m.update(open(filename, 'rb').read())
    d = m.hexdigest()
    if d != md5str:
        warnings.warn("md5sum mismatch for file %s: wanted %s; got %s" % (filename, md5str, d))
    
osarches=(("osx", ("i386", "i386-tiger", "x86_64"), ("tgz", )),
          ("linux", ("i686", "x86_64"), ("tgz", )),
          ("win32", ("i386", "x86_64"), ("zip", )),
          ("sunos5", ("i86pc", "x86_64"), ("tgz", )),
          ("src", ("src", ), ("tar.gz", "zip")), )

# KLUDGE: this will need constant editing.
versions = ("1.4.2", "1.5.1", "latest")

url_format = "http://downloads.mongodb.org/%s/mongodb-%s-%s.%s"
filename_format = "mongodb-%s-%s.%s"

def do_it():
    for version in versions:
        for (os, architectures, archives) in osarches:
            for architecture in architectures:
                for archive in archives:
                    osarch = os + '-' + architecture if architecture != 'src' else 'src'
                    # ugh.
                    if architecture == 'src' and version == 'latest':
                        if archive == 'tar.gz':
                            archive2 = 'tarball'
                        elif archive == 'zip':
                            archive2 == 'zipball'
                        url = "http://github.com/mongodb/mongo/"+archive2+"/master"
                        version2 = "master"
                    else:
                        version2 = version if architecture != 'src' else 'r'+version
                        url = url_format % (os, osarch, version2, archive)
                    # ugh ugh
                    md5url = url+'.md5' if architecture != 'src' else None
                    filename = filename_format % (osarch, version2, archive)
                    report(url, filename)
                    open(filename, 'w').write(urllib2.urlopen(url).read())
                    if md5url:
                        print "fetching md5 url " + md5url
                        md5str = urllib2.urlopen(md5url).read()
                        checkmd5(md5str, filename)

    # FIXME: in principle, the doc PDFs could be out of date.
    docs_url = time.strftime("http://downloads.mongodb.org/docs/mongodb-docs-%Y-%m-%d.pdf")
    docs_filename = time.strftime("mongodb-docs-%Y-%m-%d.pdf")
    report(docs_url, docs_filename)
    open(docs_filename, 'w').write(urllib2.urlopen(docs_url).read())

    # Drivers... FIXME: drivers.
    #langs=("c", "java", "python", "php", "perl")

if len(sys.argv) > 1: 
    dir=sys.argv[1]
    os.makedirs(dir)
    os.chdir(dir)

print """NOTE: the md5sums for all the -latest tarballs are out of
date.  You will probably see warnings as this script runs.  (If you
don't, feel free to delete this note.)"""
do_it()
