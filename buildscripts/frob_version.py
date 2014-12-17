#!/usr/bin/env python

from __future__ import with_statement
import tempfile
import sys
import re
import os

def opentemp(basename):
    # The following doesn't work in python before 2.6
#    return tempfile.NamedTemporaryFile('w', -1, ".XXXXXX", basename, '.', False)
    fname = basename +".TMP"
    if os.path.exists(fname):
        raise "not clobbering file %s" % fname
    return open(fname, 'w')

def frob_debian_changelog(version):
    fname = 'debian/changelog'
    with opentemp(fname) as o:
        with open(fname) as i:
            lineno = 0
            for line in i:
                if lineno == 0:
                    newline = re.sub(r'\([^)]*\)', '('+version+')', line)
                    o.write(newline)
                else:
                    o.write(line)
        os.rename(o.name, fname)
        
def frob_rpm_spec(version):
    fname = 'rpm/mongo.spec'
    with opentemp(fname) as o:
        with open(fname) as i:
            frobbed = False
            for line in i:
                if frobbed:
                    o.write(line)
                else:
                    if line.find('Version:') == 0:
                        print >> o, 'Version: ' + version
                        frobbed = True
                    else:
                        o.write(line)
        os.rename(o.name, fname)

def frob_stdafx_cpp(version):
    fname = 'stdafx.cpp'
    with opentemp(fname) as o:
        with open(fname) as i:
            frobbed = False
            for line in i:
                if frobbed:
                    o.write(line)
                else:
                    if re.search(r'const.*char.*versionString\[\].*=', line):
                        o.write('    const char versionString[] = "%s";' % version)
                    else:
                        o.write(line)
        os.rename(o.name, fname)

(progname, version) = sys.argv
if version is None:
    print >> sys.stderr, 'usage: %s VERSION' % progname
    sys.exit(1)
frob_debian_changelog(version)
frob_rpm_spec(version)
## I don't yet know what-all cares about the versionString inside the
## mongo code, so I'm not actually calling this yet.
# frob_stdafx_cpp(version)
