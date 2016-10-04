#!/usr/bin/env python

import re
import sys
import os
import tempfile
import subprocess
import json
import urlparse
import tarfile
import signal
import threading
import traceback
import shutil
import errno
from contextlib import closing
# To ensure it exists on the system
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


def get_version_parts(version, for_sorting=False):
    """Returns a list containing the components of the version string
    as numeric values. This function can be used for numeric sorting
    of version strings such as '2.6.0-rc1' and '2.4.0' when the
    'for_sorting' parameter is specified as true."""

    RC_OFFSET = -100
    version_parts = re.split(r'\.|-', version)

    if version_parts[-1] == "pre":
        # Prior to improvements for how the version string is managed within the server
        # (SERVER-17782), the binary archives would contain a trailing "-pre".
        version_parts.pop()

    if version_parts[-1].startswith("rc"):
        # RC versions are weighted down to allow future RCs and general
        # releases to be sorted in ascending order (e.g., 2.6.0-rc1,
        # 2.6.0-rc2, 2.6.0).
        version_parts[-1] = int(version_parts[-1][2:]) + RC_OFFSET
    elif version_parts[0].startswith("v") and version_parts[-1] == "latest":
        version_parts[0] = version_parts[0][1:]
        # The "<branchname>-latest" versions are weighted the highest when a particular major
        # release is requested.
        version_parts[-1] = float("inf")
    elif for_sorting:
        # We want to have the number of components in the resulting version parts match the number
        # of components in the 'version' string if we aren't going to be using them for sorting.
        # Otherwise, we append an additional 0 to non-RC releases so that version lists like
        # [2, 6, 0, -100] and [2, 6, 0, 0] sort in ascending order.
        version_parts.append(0)

    return [float(part) for part in version_parts]


def download_file(url, file_name):
    """Returns True if download was successful. Raises error if download fails."""
    proc = subprocess.Popen(["curl",
                             "-L", "--silent",
                             "--retry", "5",
                             "--retry-max-time", "600",
                             "--max-time", "120",
                             "-o", file_name,
                             url],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    proc.communicate()
    error_code = proc.returncode
    if not error_code:
        error_code = proc.wait()
    if not error_code:
        return True

    raise Exception("Failed to download %s with error %d" % (url, error_code))


class MultiVersionDownloader :

    def __init__(self, install_dir, link_dir, edition, platform_arch):
        self.install_dir = install_dir
        self.link_dir = link_dir
        self.edition = edition
        match = re.compile("(.*)\/(.*)").match(platform_arch)
        if match:
            self.platform_arch = match.group(1).lower() + "_" + match.group(2).lower()
        else:
            self.platform_arch = platform_arch.lower()
        self._links = None

    @property
    def links(self):
        if self._links is None:
            self._links = self.download_links()
        return self._links

    def download_links(self):
        temp_file = tempfile.mktemp()
        download_file("https://downloads.mongodb.org/full.json", temp_file)
        with open(temp_file) as f:
            full_json = json.load(f)
        os.remove(temp_file)
        if 'versions' not in full_json:
            raise Exception("No versions field in JSON: \n" + str(full_json))

        links = {}
        for json_version in full_json['versions']:
            if 'version' in json_version and 'downloads' in json_version:
                for download in json_version['downloads']:
                    if 'target' in download and 'edition' in download and \
                        download['target'] == self.platform_arch and \
                        download['edition'] == self.edition:
                            links[json_version['version']] = download['archive']['url']

        return links

    def download_version(self, version):

        try:
            os.makedirs(self.install_dir)
        except OSError as exc:
            if exc.errno == errno.EEXIST and os.path.isdir(self.install_dir):
                pass
            else: raise

        urls = []
        requested_version_parts = get_version_parts(version)
        for link_version, link_url in self.links.iteritems():
            link_version_parts = get_version_parts(link_version)
            if link_version_parts[:len(requested_version_parts)] == requested_version_parts:
                # The 'link_version' is a candidate for the requested 'version' if
                #   (a) it is a prefix of the requested version, or if
                #   (b) it is the "<branchname>-latest" version and the requested version is for a
                #       particular major release.
                # This is equivalent to the 'link_version' having components equal to all of the
                # version parts that make up 'version'.
                if "-" in version:
                    # The requested 'version' contains a hyphen, so we only consider exact matches
                    # to that version.
                    if link_version != version:
                        continue
                urls.append((link_version, link_url))

        if len(urls) == 0:
            raise Exception("Cannot find a link for version %s, versions %s found." \
                % (version, self.links))

        urls.sort(key=lambda (version, _): get_version_parts(version, for_sorting=True))
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
            print "Downloading data for version %s (%s)..." % (version, full_version)
            print "Download url is %s" % url

            temp_dir = tempfile.mkdtemp()
            temp_file = tempfile.mktemp(suffix=file_suffix)
            download_file(url, temp_file)

            print "Uncompressing data for version %s (%s)..." % (version, full_version)
            first_file = ''
            if file_suffix == ".zip":
                # Support .zip downloads, used for Windows binaries.
                with zipfile.ZipFile(temp_file) as zf:
                    # Use the name of the root directory in the archive as the name of the directory
                    # to extract the binaries into inside 'self.install_dir'. The name of the root
                    # directory nearly always matches the parsed URL text, with the exception of
                    # versions such as "v3.2-latest" that instead contain the githash.
                    first_file = zf.namelist()[0]
                    zf.extractall(temp_dir)
            elif file_suffix == ".tgz":
                # Support .tgz downloads, used for Linux binaries.
                with closing(tarfile.open(temp_file, 'r:gz')) as tf:
                    # Use the name of the root directory in the archive as the name of the directory
                    # to extract the binaries into inside 'self.install_dir'. The name of the root
                    # directory nearly always matches the parsed URL text, with the exception of
                    # versions such as "v3.2-latest" that instead contain the githash.
                    first_file = tf.getnames()[0]
                    tf.extractall(path=temp_dir)
            else:
                raise Exception("Unsupported file extension %s" % file_suffix)

            # Sometimes the zip will contain the root directory as the first file and
            # os.path.dirname() will return ''.
            extract_dir = os.path.dirname(first_file)
            if not extract_dir:
                extract_dir = first_file
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
supports community and enterprise builds.

Usage: setup_multiversion_mongodb.py INSTALL_DIR LINK_DIR EDITION PLATFORM_AND_ARCH VERSION1 [VERSION2 VERSION3 ...]

EDITION is one of the following:
    base (generic community builds)
    enterprise
    targeted (platform specific community builds, includes SSL)
PLATFORM_AND_ARCH can be specified with just a platform, i.e., OSX, if it is supported.

Ex: setup_multiversion_mongodb.py ./install ./link base "Linux/x86_64" "2.0.6" "2.0.3-rc0" "2.0" "2.2" "2.3"
Ex: setup_multiversion_mongodb.py ./install ./link enterprise "OSX" "2.4" "2.2"

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
    if len(args) == 0: raise_exception("Missing EDITION")

    edition = args[0]
    if edition not in ['base', 'enterprise', 'targeted']:
        raise Exception("Unsupported edition %s" % edition)

    args = args[1:]
    if len(args) == 0: raise_exception("Missing PLATFORM_AND_ARCH")

    platform_arch = args[0]

    args = args[1:]

    if len(args) == 0: raise_exception("Missing VERSION1")

    versions = args

    return (MultiVersionDownloader(install_dir, link_dir, edition, platform_arch), versions)

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
