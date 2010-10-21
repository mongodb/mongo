#!/usr/bin/env python

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

written_files = []
def get(url, filename):
    # A little safety check.
    if filename in written_files:
        raise Exception('not overwriting file %s (already written in this session)' % filename)
    else:
        written_files.append(filename)
    print "downloading %s to %s" % (url, filename)    
    open(filename, 'w').write(urllib2.urlopen(url).read())


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

def core_server():
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
                    get(url, filename)
                    if md5url:
                        print "fetching md5 url " + md5url
                        md5str = urllib2.urlopen(md5url).read()
                        checkmd5(md5str, filename)

def drivers():
    # Drivers... FIXME: drivers.
    driver_url_format = "http://github.com/mongodb/mongo-%s-driver/%s/%s"
    driver_filename_format = "mongo-%s-driver-%s.%s"
    drivers=(("python", ("1.6", "master"), ("zipball", "tarball"), None),
             ("ruby", ("0.20", "master"), ("zipball", "tarball"), None),
             ("c", ("v0.1", "master"), ("zipball", "tarball"), None),
             # FIXME: PHP, Java, and Csharp also have zips and jars of
             # precompiled relesaes.
             ("php", ("1.0.6", "master"), ("zipball", "tarball"), None),
             ("java", ("r1.4", "r2.0rc1", "master"), ("zipball", "tarball"), None),
             # And Csharp is in a different github place, too.
             ("csharp", ("0.82.2", "master"), ("zipball", "tarball"),
              "http://github.com/samus/mongodb-%s/%s/%s"),
             )

    for (lang, releases, archives, url_format) in drivers:
        for release in releases:
            for archive in archives:
                url = (url_format if url_format else driver_url_format) % (lang, archive, release) 
                if archive == 'zipball':
                    extension = 'zip'
                elif archive == 'tarball':
                    extension = 'tgz'
                else:
                    raise Exception('unknown archive format %s' % archive)
                filename = driver_filename_format % (lang, release, extension)
                get(url, filename)
        # ugh ugh ugh
        if lang == 'csharp' and release != 'master':
            url = 'http://github.com/downloads/samus/mongodb-csharp/MongoDBDriver-Release-%.zip' % (release)
            filename = 'MongoDBDriver-Release-%.zip' % (release)
            get(url, filename)
        if lang == 'java' and release != 'master':
            get('http://github.com/downloads/mongodb/mongo-java-driver/mongo-%s.jar' % (release), 'mongo-%s.jar' % (release))
        # I have no idea what's going on with the PHP zipfiles.
        if lang == 'php' and release == '1.0.6':
            get('http://github.com/downloads/mongodb/mongo-php-driver/mongo-1.0.6-php5.2-osx.zip', 'mongo-1.0.6-php5.2-osx.zip')
            get('http://github.com/downloads/mongodb/mongo-php-driver/mongo-1.0.6-php5.3-osx.zip', 'mongo-1.0.6-php5.3-osx.zip')

def docs():
    # FIXME: in principle, the doc PDFs could be out of date.
    docs_url = time.strftime("http://downloads.mongodb.org/docs/mongodb-docs-%Y-%m-%d.pdf")
    docs_filename = time.strftime("mongodb-docs-%Y-%m-%d.pdf")
    get(docs_url, docs_filename)

def extras():
    # Extras
    extras = ("http://media.mongodb.org/zips.json", )
    for extra in extras:
        if extra.rfind('/') > -1:
            filename = extra[extra.rfind('/')+1:]
        else: 
            raise Exception('URL %s lacks a slash?' % extra)
        get(extra, filename)

if len(sys.argv) > 1: 
    dir=sys.argv[1]
    os.makedirs(dir)
    os.chdir(dir)

print """NOTE: the md5sums for all the -latest tarballs are out of
date.  You will probably see warnings as this script runs.  (If you
don't, feel free to delete this note.)"""
core_server()
drivers()
docs()
extras()
