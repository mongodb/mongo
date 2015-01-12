#!/usr/bin/python

import re
import sys
import os
import tempfile
import urllib2
import subprocess
import tarfile
import zipfile
import shutil
import errno
# To ensure it exists on the system
import gzip
import argparse

#
# Useful script for installing multiple versions of MongoDB on a machine
# Only really tested/works on Linux.
#

def version_tuple(version):
    """Returns a version tuple that can be used for numeric sorting
    of version strings such as '2.6.0-rc1' and '2.4.0'"""

    RC_OFFSET = -100
    version_parts = re.split(r'\.|-', version[0])

    if version_parts[-1].startswith("rc"):
        rc_part = version_parts.pop()
        rc_part = rc_part.split('rc')[1]

        # RC versions are weighted down to allow future RCs and general
        # releases to be sorted in ascending order (e.g., 2.6.0-rc1,
        # 2.6.0-rc2, 2.6.0).
        version_parts.append(int(rc_part) + RC_OFFSET)
    else:
        # Non-RC releases have an extra 0 appended so version tuples like
        # (2, 6, 0, -100) and (2, 6, 0, 0) sort in ascending order.
        version_parts.append(0)

    return tuple(map(int, version_parts))

class MultiVersionDownloaderBase : 

    def download_version(self, version):

        try:
            os.makedirs(self.install_dir)
        except OSError as exc:
            if exc.errno == errno.EEXIST and os.path.isdir(self.install_dir):
                pass
            else: raise

        url, full_version = self.gen_url(version)

        # this extracts the filename portion of the URL, without the extension.
        # for example: ttp://downloads.mongodb.org/osx/mongodb-osx-x86_64-2.4.12.tgz
        # extract_dir will become mongodb-osx-x86_64-2.4.12
        extract_dir = url.split("/")[-1][:-4]

        # only download if we don't already have the directory
        already_downloaded = os.path.isdir(os.path.join( self.install_dir, extract_dir))
        if already_downloaded:
            print "Skipping download for version %s (%s) since the dest already exists '%s'" \
                % (version, full_version, extract_dir)
        else:
            temp_dir = tempfile.mkdtemp()
            temp_file = tempfile.mktemp(suffix=".tgz")

            data = urllib2.urlopen(url)

            print "Downloading data for version %s (%s) from %s..." % (version, full_version, url)

            with open(temp_file, 'wb') as f:
                f.write(data.read())
                print "Uncompressing data for version %s (%s)..." % (version, full_version)

            try:
                tf = tarfile.open(temp_file, 'r:gz')
                tf.extractall(path=temp_dir)
                tf.close()
            except:
                # support for windows
                zfile = zipfile.ZipFile(temp_file)
                try:
                    if not os.path.exists(temp_dir):
                        os.makedirs(temp_dir)
                    for name in zfile.namelist():
                        _, filename = os.path.split(name)
                        print "Decompressing " + filename + " on " + temp_dir
                        zfile.extract(name, temp_dir)
                except:
                    zfile.close()
                    raise
                zfile.close()
            temp_install_dir = os.path.join(temp_dir, extract_dir)
            try:
                os.stat(temp_install_dir)
            except:
                dir = os.listdir(temp_dir)
                # TODO confirm that there is one and only one directory entry
                os.rename(os.path.join(temp_dir,dir[0]),temp_install_dir)
            shutil.move(temp_install_dir, self.install_dir)
            shutil.rmtree(temp_dir)
            try:
                os.remove(temp_file)
            except Exception as e:
                print e
                pass
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
            # support for windows
            if executable.endswith(".exe") or executable.endswith(".pdb"):
                link_name = "%s-%s.%s" % (executable[:-4], version, executable[len(executable)-3:])

            try:
                os.symlink(os.path.join(installed_dir, "bin", executable),\
                           os.path.join(self.link_dir, link_name))
            except Exception as exc:
                try:
                    # support for windows
                    shutil.copy2(os.path.join(installed_dir, "bin", executable),\
                               os.path.join(self.link_dir, link_name))
                except:
                    if exc.errno == errno.EEXIST:
                        pass
                    else:
                        raise

class MultiVersionDownloader(MultiVersionDownloaderBase) :

    def __init__(self, install_dir, link_dir, platform):
        self.install_dir = install_dir
        self.link_dir = link_dir
        match = re.compile("(.*)\/(.*)").match(platform)
        self.platform = match.group(1)
        self.arch = match.group(2)
        self._links = None

    @property
    def links(self):
        if self._links is None:
            self._links = self.download_links()
        return self._links

    def gen_url(self, version):
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

        urls.sort(key=version_tuple)
        full_version = urls[-1][0]
        url = urls[-1][1]
        return url, full_version

    def download_links(self):
        href = "http://dl.mongodb.org/dl/%s/%s" \
               % (self.platform, self.arch)

        html = urllib2.urlopen(href).read()
        links = {}
        for line in html.split():
            match = None
            for ext in ["tgz", "zip"]:
                match = re.compile("http:\/\/downloads\.mongodb\.org\/%s/mongodb-%s-%s-([^\"]*)\.%s" \
                    % (self.platform, self.platform, self.arch, ext)).search(line)
                if match != None:
                    break

            if match == None:
                continue
            link = match.group(0)
            version = match.group(1)
            links[version] = link

        return links


class LatestMultiVersionDownloader(MultiVersionDownloaderBase) :

    def __init__(self, install_dir, link_dir, platform, use_ssl, os):
        self.install_dir = install_dir
        self.link_dir = link_dir
        match = re.compile("(.*)\/(.*)").match(platform)
        self.platform = match.group(1)
        self.arch = match.group(2)
        self._links = None
        self.use_ssl = use_ssl
        self.os = os

    def gen_url(self, version):
        ext = "tgz"
        if "win" in self.platform:
            ext = "zip"
        if self.use_ssl:
            if version == "2.4":
                enterprise_string = "subscription"
            else:
                enterprise_string = "enterprise"
            full_version = self.os + "-v" + version + "-latest"
            url = "http://downloads.10gen.com/%s/mongodb-%s-%s-%s-%s.%s" % ( self.platform, self.platform, self.arch, enterprise_string, full_version, ext )
        else:
            full_version = "v" + version + "-latest"
            url = "http://downloads.mongodb.org/%s/mongodb-%s-%s-%s.%s" % ( self.platform, self.platform, self.arch, full_version, ext )
        return url, full_version

CL_HELP_MESSAGE = \
"""
Downloads and installs particular mongodb versions (each binary is renamed to include its version) 
into an install directory and symlinks the binaries with versions to another directory.

Usage: setup_multiversion_mongodb.py INSTALL_DIR LINK_DIR PLATFORM_AND_ARCH VERSION1 [VERSION2 VERSION3 ...]

Ex: setup_multiversion_mongodb.py ./install ./link "Linux/x86_64" "2.0.6" "2.0.3-rc0" "2.0" "2.2" "2.3"
Ex: setup_multiversion_mongodb.py ./install ./link "OSX/x86_64" "2.4" "2.2"

After running the script you will have a directory structure like this:
./install/[mongodb-osx-x86_64-2.4.9, mongodb-osx-x86_64-2.2.7]
./link/[mongod-2.4.9, mongod-2.2.7, mongo-2.4.9...]

You should then add ./link/ to your path so multi-version tests will work.

Note: If "rc" is included in the version name, we'll use the exact rc, otherwise we'll pull the highest non-rc
version compatible with the version specified.
"""

def parse_cl_args(args):

    parser = argparse.ArgumentParser(description=CL_HELP_MESSAGE)

    def raise_exception(msg):
        print CL_HELP_MESSAGE
        raise Exception(msg)

    parser.add_argument('install_dir', action="store" )
    parser.add_argument('link_dir', action="store" )
    parser.add_argument('platform_and_arch', action="store" )
    parser.add_argument('--latest', action="store_true" )
    parser.add_argument('--use-ssl', action="store_true" )
    parser.add_argument('--os', action="store" )
    parser.add_argument('version', action="store", nargs="+" )

    args = parser.parse_args()

    if re.compile(".*\/.*").match(args.platform_and_arch) == None:
        raise_exception("PLATFORM_AND_ARCH isn't of the correct format")

    if args.latest:
        if not args.os:
            raise_exception("using --use-ssl requires an --os parameter")
        return (LatestMultiVersionDownloader(args.install_dir, args.link_dir, args.platform_and_arch, args.use_ssl, args.os), args.version)
    else:
        if args.use_ssl:
            raise_exception("you can only use --use-ssl when using --latest")
        return (MultiVersionDownloader(args.install_dir, args.link_dir, args.platform_and_arch), args.version)

def main():

    downloader, versions = parse_cl_args(sys.argv[1:])

    for version in versions:
        downloader.download_version(version)



if __name__ == '__main__':
  main()

