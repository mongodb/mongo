#!/usr/bin/env python

"""
Command line utility returns the URL of the most recent archive file
satisfying given version, edition, and operating system requirements.
"""

import argparse
import json
import sys
import urllib2

url_current = "http://downloads.mongodb.org/current.json"
url_full = "http://downloads.mongodb.org/full.json"

parser = argparse.ArgumentParser()
parser.add_argument("--arch", help="processor architecture (e.g. 'x86_64', 'arm64')")
parser.add_argument("--edition", help="edition of MongoDB to use (e.g. 'targeted', 'enterprise'); defaults to 'base'")
parser.add_argument("--target", help="system in use (e.g. 'ubuntu1204', 'windows_x86_64-2008plus-ssl', 'rhel71')")
parser.add_argument("--version", help="version branch (e.g. '2.6', '3.2.8-rc1', 'latest')")
opts = parser.parse_args()

if not opts.edition:
  opts.edition = "base"
if not opts.arch:
  sys.exit("must specify arch")
if not opts.target:
  sys.exit("must specify target")
if not opts.version:
  sys.exit("must specify version")

# prior to the 2.6 branch, the enterprise edition was called 'subscription'
if opts.version == "2.4" and opts.edition == "enterprise":
  opts.edition = "subscription"

def isCorrectVersion(version):
  actual = version["version"].split(".")
  desired = opts.version.split(".")
  for i in range(len(desired)):
    if desired[i] and not actual[i] == desired[i]:
      return False
  return True

def isCorrectDownload(download):
  return download["edition"] == opts.edition and download["target"] == opts.target and download["arch"] == opts.arch

def locateUrl(specs, override):
  versions = specs["versions"]
  if not override:
    versions = filter(isCorrectVersion, versions)
  for item in versions:
    downloads = filter(isCorrectDownload, item["downloads"])
    urls = map(lambda download : download["archive"]["url"], downloads)
    if len(urls) > 0:
      if override:
        return urls[0].replace(item["version"], override)
      return urls[0]

override = "latest" if opts.version == "latest" else None

specs = json.load(urllib2.urlopen(url_current))
url = locateUrl(specs, override)

if not url:
  specs = json.load(urllib2.urlopen(url_full))
  url = locateUrl(specs, override)

if not url:
  sys.exit("No info for version "+opts.version+" found")

sys.stdout.write(url)
