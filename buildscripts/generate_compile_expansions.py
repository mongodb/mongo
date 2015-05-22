#!/usr/bin/env python
#
# This script generates the compile expansions file used by MCI as part of the push/
# release process.
#
# You can invoke it either with git describe:
# $ git describe | python generate_compile_expansions.py > compile_expansions.yml
# or with the version.json file
# $ python generate_compile_expansions.py version.json > compile_expansions.yml
#

import fileinput
import json
import re

# This function matches a version string and captures the "extra" part
# If the version is a release like "2.3.4" or "2.3.4-rc0", this will return
# ( None )
# If the version is a pre-release like "2.3.4-325-githash" or "2.3.4-pre-", this will return
# ( "-pre-" ) or ( "-325-githash" )
# If the version begins with the letter 'r', it will also match, e.g.
# r2.3.4, r2.3.4-rc0, r2.3.4-git234, r2.3.4-rc0-234-githash
# If the version is invalid (i.e. doesn't start with "2.3.4" or "2.3.4-rc0", this will return
# False
def match_verstr(verstr):
    res = re.match(r'^r?(?:\d+\.\d+\.\d+(?:-rc\d+)?)(-.*)?', verstr)
    if not res:
        return False
    return res.groups()

input_obj = fileinput.input()
version_line = input_obj.readline()
version_parts = match_verstr(version_line)
if not version_parts:
    if input_obj.filename().endswith(".json"):
        version_data_buf = "".join([ version_line ] + [ l for l in input_obj ])
        try:
            version_data = json.loads(version_data_buf)
        except Exception as e:
            print "Unable to load json file: %s" % e
            exit(1)
        version_parts = match_verstr(version_data['version'])
        version_line = version_data['version']
else:
    version_line = version_line.lstrip("r").rstrip()

# If the version still doesn't match, give up and print an error message!
if not version_parts:
    print "Unable to parse version data!"
    exit(1)

if version_parts[0]:
    print "suffix: latest"
    print "src_suffix: latest"
else:
    print "suffix: {0}".format(version_line)
    print "src_suffix: r{0}".format(version_line)

print "version: {0}".format(version_line)
