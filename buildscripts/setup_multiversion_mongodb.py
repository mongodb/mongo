#!/usr/bin/env python

import re
import sys
import os
import tempfile
import urllib2
import urlparse
import subprocess
import tarfile
import signal
import threading
import traceback
import shutil
import errno
from contextlib import closing
# To ensure it exists on the system
import gzip
import zipfile

#
# Useful script for installing multiple versions of MongoDB on a machine
# Only really tested/works on Linux.
#

def dump_stacks(signal, frame):
    print "======================================"
    print "DUMPING STACKS due to SIGUSR1 signal"
    print "======================================"
    threads = threading.enumerate();

    print "Total Threads: " + str(len(threads))

    for id, stack in sys._current_frames().items():
        print "Thread %d" % (id)
        print "".join(traceback.format_stack(stack))
    print "======================================"


def version_tuple(version):
    """Returns a version tuple that can be used for numeric sorting
    of version strings such as '2.6.0-rc1' and '2.4.0'"""

    RC_OFFSET = -100
    version_parts = re.split(r'\.|-', version[0])

    if version_parts[-1] == "pre":
        # Prior to improvements for how the version string is managed within the server
        # (SERVER-17782), the binary archives would contain a trailing "-pre".
        version_parts.pop()

    if version_parts[-1].startswith("rc"):
        rc_part = version_parts.pop()
        rc_part = rc_part.split('rc')[1]

        # RC versions are weighted down to allow future RCs and general
        # releases to be sorted in ascending order (e.g., 2.6.0-rc1,
        # 2.6.0-rc2, 2.6.0).
        version_parts.append(int(rc_part) + RC_OFFSET)
    elif version_parts[0].startswith("v") and version_parts[-1] == "latest":
        version_parts[0] = version_parts[0][1:]
        # The "<branchname>-latest" versions are weighted the highest when a particular major
        # release is requested.
        version_parts[-1] = float("inf")
    else:
        # Non-RC releases have an extra 0 appended so version tuples like
        # (2, 6, 0, -100) and (2, 6, 0, 0) sort in ascending order.
        version_parts.append(0)

    return tuple(map(float, version_parts))

class MultiVersionDownloader :

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

    def download_links(self):
        # This href is for community builds; enterprise builds are not browseable.
        href = "http://dl.mongodb.org/dl/%s/%s" \
               % (self.platform.lower(), self.arch)

        attempts_remaining = 5
        timeout_seconds = 10
        while True:
            try:
                html = urllib2.urlopen(href, timeout = timeout_seconds).read()
                break
            except Exception as e:
                print "fetching links failed (%s), retrying..." % e
                attempts_remaining -= 1
                if attempts_remaining == 0 :
                    raise Exception("Failed to get links after multiple retries")

        links = {}
        for line in html.split():
            match = re.compile("http:.*/%s/mongodb-%s-%s-([^\"]*)\.(tgz|zip)" \
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
            if link_version.startswith(version) or link_version == "v%s-latest" % (version):
                # The 'link_version' is a candidate for the requested 'version' if
                #   (a) it is a prefix of the requested version, or if
                #   (b) it is the "<branchname>-latest" version and the requested version is for a
                #       particular major release.
                if "-" in version:
                    # The requested 'version' contains a hyphen, so we only consider exact matches
                    # to that version.
                    if link_version != version:
                        continue
                urls.append((link_version, link_url))

        if len(urls) == 0:
            raise Exception("Cannot find a link for version %s, versions %s found." \
                % (version, self.links))

        urls.sort(key=version_tuple)
        full_version = urls[-1][0]
        url = urls[-1][1]
        extract_dir = url.split("/")[-1][:-4]
        file_suffix = os.path.splitext(urlparse.urlparse(url).path)[1]

        # only download if we don't already have the directory
        already_downloaded = os.path.isdir(os.path.join( self.install_dir, extract_dir))
        if already_downloaded:
            print "Skipping download for version %s (%s) since the dest already exists '%s'" \
                % (version, full_version, extract_dir)
        else:
            temp_dir = tempfile.mkdtemp()
            temp_file = tempfile.mktemp(suffix=file_suffix)
    
            data = urllib2.urlopen(url)
    
            print "Downloading data for version %s (%s)..." % (version, full_version)
            print "Download url is %s" % url
    
            with open(temp_file, 'wb') as f:
                f.write(data.read())
                print "Uncompressing data for version %s (%s)..." % (version, full_version)
    
            if file_suffix == ".zip":
                # Support .zip downloads, used for Windows binaries.
                with zipfile.ZipFile(temp_file) as zf:
                    # Use the name of the root directory in the archive as the name of the directory
                    # to extract the binaries into inside 'self.install_dir'. The name of the root
                    # directory nearly always matches the parsed URL text, with the exception of
                    # versions such as "v3.2-latest" that instead contain the githash.
                    extract_dir = os.path.dirname(zf.namelist()[0])
                    zf.extractall(temp_dir)
            elif file_suffix == ".tgz":
                # Support .tgz downloads, used for Linux binaries.
                with closing(tarfile.open(temp_file, 'r:gz')) as tf:
                    # Use the name of the root directory in the archive as the name of the directory
                    # to extract the binaries into inside 'self.install_dir'. The name of the root
                    # directory nearly always matches the parsed URL text, with the exception of
                    # versions such as "v3.2-latest" that instead contain the githash.
                    extract_dir = os.path.dirname(tf.getnames()[0])
                    tf.extractall(path=temp_dir)
            else:
                raise Exception("Unsupported file extension %s" % file_suffix)
    
            temp_install_dir = os.path.join(temp_dir, extract_dir)
    
            # We may not have been able to determine whether we already downloaded the requested
            # version due to the ambiguity in the parsed URL text, so we check for it again using
            # the adjusted 'extract_dir' value.
            already_downloaded = os.path.isdir(os.path.join(self.install_dir, extract_dir))
            if not already_downloaded:
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

            executable_name, executable_extension = os.path.splitext(executable)
            link_name = "%s-%s%s" % (executable_name, version, executable_extension)

            try:
                executable = os.path.join(installed_dir, "bin", executable)
                executable_link = os.path.join(self.link_dir, link_name)
                if os.name == "nt":
                    # os.symlink is not supported on Windows, use a direct method instead.
                    def symlink_ms(source, link_name):
                        import ctypes
                        csl = ctypes.windll.kernel32.CreateSymbolicLinkW
                        csl.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
                        csl.restype = ctypes.c_ubyte
                        flags = 1 if os.path.isdir(source) else 0
                        if csl(link_name, source.replace('/', '\\'), flags) == 0:
                            raise ctypes.WinError()
                    os.symlink = symlink_ms
                os.symlink(executable, executable_link)
            except OSError as exc:
                if exc.errno == errno.EEXIST:
                    pass
                else: raise


CL_HELP_MESSAGE = \
"""
Downloads and installs particular mongodb versions (each binary is renamed to include its version)
into an install directory and symlinks the binaries with versions to another directory. This script
only supports community builds, not enterprise builds.

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

    # Listen for SIGUSR1 and dump stack if received.
    try:
        signal.signal(signal.SIGUSR1, dump_stacks)
    except AttributeError:
        print "Cannot catch signals on Windows"

    downloader, versions = parse_cl_args(sys.argv[1:])

    for version in versions:
        downloader.download_version(version)



if __name__ == '__main__':
  main()

