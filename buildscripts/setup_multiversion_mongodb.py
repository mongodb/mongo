#!/usr/bin/python

import re
import sys
import os
import tempfile
import urllib2
import subprocess
import tarfile
import shutil
import errno
# To ensure it exists on the system
import gzip

#
# Useful script for installing multiple versions of MongoDB on a machine
# Only really tested/works on Linux.
#

class MultiVersionDownloader :

    def __init__(self, install_dir, link_dir, platform):
        self.install_dir = install_dir
        self.link_dir = link_dir
        match = re.compile("(.*)\/(.*)").match(platform)
        self.platform = match.group(1)
        self.arch = match.group(2)
        self.links = self.download_links()

    def download_links(self):
        href = "http://dl.mongodb.org/dl/%s/%s" \
               % (self.platform.lower(), self.arch)

        html = urllib2.urlopen(href).read()

        links = {}
        for line in html.split():
            match = re.compile("http:\/\/downloads\.mongodb\.org\/%s/mongodb-%s-%s-([^\"]*)\.tgz" \
                % (self.platform.lower(), self.platform.lower(), self.arch)).search(line)

            if match == None: continue

            link = match.group(0)
            version = match.group(1)
            links[version] = link

        return links

    def download_version(self, version):

        try:
            os.makedirs(self.install_dir)
        except OSError as exc:
            if exc.errno == errno.EEXIST and os.path.isdir(self.install_dir):
                pass
            else: raise

        urls = []
        for link_version, link_url in self.links.iteritems():
            if link_version.startswith(version):
                # If we have a "-" in our version, exact match only
                if version.find("-") >= 0:
                    if link_version != version: continue
                elif link_version.find("-") >= 0:
                    continue

                urls.append((link_version, link_url))

        if len(urls) == 0:
            raise Exception("Cannot find a link for version %s, versions %s found." \
                % (version, self.links))

        urls.sort()
        full_version = urls[-1][0]
        url = urls[-1][1]

        temp_dir = tempfile.mkdtemp()
        temp_file = tempfile.mktemp(suffix=".tgz")

        data = urllib2.urlopen(url)

        print "Downloading data for version %s (%s)..." % (version, full_version)

        with open(temp_file, 'wb') as f:
            f.write(data.read())
            print "Uncompressing data for version %s (%s)..." % (version, full_version)

        # Can't use cool with syntax b/c of python 2.6
        tf = tarfile.open(temp_file, 'r:gz')

        try:
            tf.extractall(path=temp_dir)
        except:
            tf.close()
            raise

        tf.close()

        extract_dir = os.listdir(temp_dir)[0]
        temp_install_dir = os.path.join(temp_dir, extract_dir)

        shutil.move(temp_install_dir, self.install_dir)

        shutil.rmtree(temp_dir)
        os.remove(temp_file)

        self.symlink_version(version, os.path.abspath(os.path.join(self.install_dir, extract_dir)))


    def symlink_version(self, version, installed_dir):

        try:
            os.makedirs(self.link_dir)
        except OSError as exc:
            if exc.errno == errno.EEXIST and os.path.isdir(self.link_dir):
                pass
            else: raise

        for executable in os.listdir(os.path.join(installed_dir, "bin")):

            link_name = "%s-%s" % (executable, version)

            os.symlink(os.path.join(installed_dir, "bin", executable),\
                       os.path.join(self.link_dir, link_name))


CL_HELP_MESSAGE = \
"""
Downloads and installs particular mongodb versions into an install directory and symlinks the binaries with versions to
another directory.

Usage: install_multiversion_mongodb.sh INSTALL_DIR LINK_DIR PLATFORM_AND_ARCH VERSION1 [VERSION2 VERSION3 ...]

Ex: install_multiversion_mongodb.sh ./install ./link "Linux/x86_64" "2.0.6" "2.0.3-rc0" "2.0" "2.2" "2.3"

If "rc" is included in the version name, we'll use the exact rc, otherwise we'll pull the highest non-rc
version compatible with the version specified.
"""

def parse_cl_args(args):

    def raise_exception(msg):
        print CL_HELP_MESSAGE
        raise Exception(msg)

    if len(args) == 0: raise_exception("Missing INSTALL_DIR")

    install_dir = args[0]

    args = args[1:]
    if len(args) == 0: raise_exception("Missing LINK_DIR")

    link_dir = args[0]

    args = args[1:]
    if len(args) == 0: raise_exception("Missing PLATFORM_AND_ARCH")

    platform = args[0]

    args = args[1:]
    if re.compile(".*\/.*").match(platform) == None:
        raise_exception("PLATFORM_AND_ARCH isn't of the correct format")

    if len(args) == 0: raise_exception("Missing VERSION1")

    versions = args

    return (MultiVersionDownloader(install_dir, link_dir, platform), versions)

def main():

    downloader, versions = parse_cl_args(sys.argv[1:])

    for version in versions:
        downloader.download_version(version)



if __name__ == '__main__':
  main()

