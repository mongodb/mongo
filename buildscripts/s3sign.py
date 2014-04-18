#!/usr/bin/env python
#
# Generate and upload detached gpg signatures for archive files in Amazon S3
#
# Requires standard MongoDB settings.py, like so:
# bucket = "downloads.mongodb.org"
# # downloads user
# id = "xxxxx"
# key = "xxxxx"
#
# Usage: s3sign.py [ --dry-run ] [ --bucket <overridden s3 bucket> ] [ --notary-url <notary url> ] [ --key-name <key name passed to notary service> ] [ --filter <filter> ]
#


import argparse
import json
import os
import requests
import sys

sys.path.append("." )
sys.path.append(".." )
sys.path.append("../../" )
sys.path.append("../../../" )

import simples3
import settings
import subprocess

# parse command line
#
parser = argparse.ArgumentParser(description='Sign MongoDB S3 Files')
parser.add_argument('--dry-run', action='store_true', required=False, help='Do not write anything to S3', default = False);
parser.add_argument('--bucket', required = False, help='Override bucket in settings.py', default = settings.bucket);
parser.add_argument('--notary-url', required=False, help='URL base for notary service', default = 'http://localhost:5000');
parser.add_argument('--key-name', required=False, help='Key parameter to notary service', default = 'test');
parser.add_argument('--filter', required=False,
                    help='Only sign files matching case-insensitive substring filter', default = None);
args = parser.parse_args()

notary_urlbase = args.notary_url
notary_url = notary_urlbase + '/api/sign'
notary_payload = { 'key': args.key_name, 'comment': 'Automatic archive signing'}

# check s3 for pgp signatures

def check_dir( bucket , prefix ):

    zips = {}
    sigs = {}
    for ( key , modify , etag , size ) in bucket.listdir( prefix=prefix ):
        # filtered out 
        if args.filter and args.filter.lower() not in key.lower():
            pass
        # sign it
        elif key.endswith(".tgz" ) or key.endswith(".zip" ) or key.endswith(".tar.gz" ) or key.endswith("md5"):
            # generate signature
            files = {'file': (key, bucket.get(key))}
            response_json = {}
            try:
              r = requests.post(notary_url, files=files, data=notary_payload, 
                                headers = { "Accept": "application/json" })
              # get url for signature file
              response_json = json.loads(r.text)
            except Exception as e:
              print('error contacting signing service for %s:\n%s' % (key, e.message))
              continue
            if 'permalink' in response_json:
              signature_url = response_json['permalink']
              try:
                signature = requests.get(notary_urlbase + signature_url).text
                zips[key] = signature
              except Exception as e: 
                print('error downloading signature from signing service for %s:\n%s' % (key, e.message))
            else:
              print('error from signing service for %s:\n%s' % (key, response_json.get('message')))
        # signatures
        elif key.endswith(".sig" ) or key.endswith(".asc" ):
            sigs[key] = True
        # file types we don't need to sign
        elif key.endswith(".msi" ) or key.endswith(".deb") or key.endswith(".rpm"):
            pass
        else:
            print("unknown file type: %s" % key)

    for x in zips:
        m = x + ".sig"
        if m in sigs:
            continue

        print("need to do: " + x + " to " + m )
        if not args.dry_run:
          bucket.put( m , zips[x] , acl="public-read" )


def run():

    bucket = simples3.S3Bucket( args.bucket , settings.id , settings.key )

    for x in [ "osx" , "linux" , "win32" , "sunos5" , "src" ]:
        check_dir( bucket , x )


if __name__ == "__main__":
    run()
