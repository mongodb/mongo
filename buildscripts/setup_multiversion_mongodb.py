#!/usr/bin/env python

"""Install multiple versions of MongoDB on a machine."""

from __future__ import print_function

import contextlib
import errno
import json
import optparse
import os
import re
import shutil
import signal
import sys
import tarfile
import tempfile
import threading
import traceback
import urlparse
import zipfile

import requests
import requests.exceptions


def dump_stacks(_signal_num, _frame):
    """Dump stacks when SIGUSR1 is received."""
    print("======================================")
    print("DUMPING STACKS due to SIGUSR1 signal")
    print("======================================")
    threads = threading.enumerate()

    print("Total Threads: {:d}".format(len(threads)))

    for tid, stack in sys._current_frames().items():
        print("Thread {:d}".format(tid))
        print("".join(traceback.format_stack(stack)))
    print("======================================")


def get_version_parts(version, for_sorting=False):
    """Returns a list containing the components of the version string
    as numeric values. This function can be used for numeric sorting
    of version strings such as '2.6.0-rc1' and '2.4.0' when the
    'for_sorting' parameter is specified as true."""

    RC_OFFSET = -100
    version_parts = re.split(r"\.|-", version)

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


def download_file(url, file_name, download_retries=5):
    """Returns True if download was successful. Raises error if download fails."""

    while download_retries > 0:

        with requests.Session() as session:
            adapter = requests.adapters.HTTPAdapter(max_retries=download_retries)
            session.mount(url, adapter)
            response = session.get(url, stream=True)
            response.raise_for_status()

            with open(file_name, "wb") as file_handle:
                try:
                    for block in response.iter_content(1024 * 1000):
                        file_handle.write(block)
                except requests.exceptions.ChunkedEncodingError as err:
                    download_retries -= 1
                    if download_retries == 0:
                        raise Exception("Incomplete download for URL {}: {}".format(url, err))
                    continue

        # Check if file download was completed.
        if "Content-length" in response.headers:
            url_content_length = int(response.headers["Content-length"])
            file_size = os.path.getsize(file_name)
            # Retry download if file_size has an unexpected size.
            if url_content_length != file_size:
                download_retries -= 1
                if download_retries == 0:
                    raise Exception("Downloaded file size ({} bytes) doesn't match content length"
                        "({} bytes) for URL {}".format(file_size, url_content_length, url))
                continue

        return True

    raise Exception("Unknown download problem for {} to file {}".format(url, file_name))


class MultiVersionDownloader(object):
    """Class to support multiversion downloads."""

    def __init__(self,
                 install_dir,
                 link_dir,
                 edition,
                 platform_arch,
                 use_latest=False):
        self.install_dir = install_dir
        self.link_dir = link_dir
        self.edition = edition.lower()
        self.platform_arch = platform_arch.lower().replace("/", "_")
        self.generic_arch = "linux_x86_64"
        self.use_latest = use_latest
        self._links = None
        self._generic_links = None

    @property
    def generic_links(self):
        """Returns a list of generic links."""
        if self._generic_links is None:
            self._links, self._generic_links = self.download_links()
        return self._generic_links

    @property
    def links(self):
        """Returns a list of links."""
        if self._links is None:
            self._links, self._generic_links = self.download_links()
        return self._links

    @staticmethod
    def is_major_minor_version(version):
        """Returns True if the version is specified as M.m."""
        if re.match(r"^\d+?\.\d+?$", version) is None:
            return False
        return True

    def download_links(self):
        """Returns the download and generic download links."""
        temp_file = tempfile.mktemp()
        download_file("https://downloads.mongodb.org/full.json", temp_file)
        with open(temp_file) as file_handle:
            full_json = json.load(file_handle)
        os.remove(temp_file)
        if "versions" not in full_json:
            raise Exception("No versions field in JSON: \n" + str(full_json))

        links = {}
        generic_links = {}
        for json_version in full_json["versions"]:
            if "version" in json_version and 'downloads' in json_version:
                version = json_version["version"]
                for download in json_version["downloads"]:
                    if "target" in download and "edition" in download:
                        if download["target"].lower() == self.platform_arch and \
                                download["edition"].lower() == self.edition:
                            links[version] = download["archive"]["url"]
                        elif download["target"].lower() == self.generic_arch and \
                                download["edition"].lower() == "base":
                            generic_links[version] = download["archive"]["url"]

        return links, generic_links

    def download_version(self, version):
        """Downloads the version specified."""

        try:
            os.makedirs(self.install_dir)
        except OSError as exc:
            if exc.errno == errno.EEXIST and os.path.isdir(self.install_dir):
                pass
            else:
                raise

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
            print("Cannot find a link for version {}, versions {} found.".format(
                version, self.links), file=sys.stderr)
            for ver, generic_url in self.generic_links.iteritems():
                parts = get_version_parts(ver)
                if parts[:len(requested_version_parts)] == requested_version_parts:
                    if "-" in version and ver != version:
                        continue
                    urls.append((ver, generic_url))
            if len(urls) == 0:
                raise Exception(
                    "No fall-back generic link available or version {}.".format(version))
            else:
                print("Falling back to generic architecture.")

        urls.sort(key=lambda (version, _): get_version_parts(version, for_sorting=True))
        full_version = urls[-1][0]
        url = urls[-1][1]
        extract_dir = url.split("/")[-1][:-4]
        file_suffix = os.path.splitext(urlparse.urlparse(url).path)[1]

        # Only download if we don't already have the directory.
        # Note, we cannot detect if 'latest' has already been downloaded, as the name
        # of the 'extract_dir' cannot be derived from the URL, since it contains the githash.
        already_downloaded = os.path.isdir(os.path.join(self.install_dir, extract_dir))
        if already_downloaded:
            print("Skipping download for version {} ({}) since the dest already exists '{}'"
                  .format(version, full_version, extract_dir))
        else:
            temp_dir = tempfile.mkdtemp()
            temp_file = tempfile.mktemp(suffix=file_suffix)

            latest_downloaded = False
            # We try to download 'v<version>-latest' if the 'version' is specified
            # as Major.minor. If that fails, we then try to download the version that
            # was specified.
            if self.use_latest and self.is_major_minor_version(version):
                latest_version = "v{}-latest".format(version)
                latest_url = url.replace(full_version, latest_version)
                print("Trying to download {}...".format(latest_version))
                print("Download url is {}".format(latest_url))
                try:
                    download_file(latest_url, temp_file)
                    full_version = latest_version
                    latest_downloaded = True
                except requests.exceptions.HTTPError:
                    print("Failed to download {}".format(latest_url))
                    pass

            if not latest_downloaded:
                print("Downloading data for version {} ({})...".format(version, full_version))
                print("Download url is {}".format(url))
                download_file(url, temp_file)

            print("Uncompressing data for version {} ({})...".format(version, full_version))
            first_file = ""
            if file_suffix == ".zip":
                # Support .zip downloads, used for Windows binaries.
                with zipfile.ZipFile(temp_file) as zip_handle:
                    # Use the name of the root directory in the archive as the name of the directory
                    # to extract the binaries into inside 'self.install_dir'. The name of the root
                    # directory nearly always matches the parsed URL text, with the exception of
                    # versions such as "v3.2-latest" that instead contain the githash.
                    first_file = zip_handle.namelist()[0]
                    zip_handle.extractall(temp_dir)
            elif file_suffix == ".tgz":
                # Support .tgz downloads, used for Linux binaries.
                with contextlib.closing(tarfile.open(temp_file, "r:gz")) as tar_handle:
                    # Use the name of the root directory in the archive as the name of the directory
                    # to extract the binaries into inside 'self.install_dir'. The name of the root
                    # directory nearly always matches the parsed URL text, with the exception of
                    # versions such as "v3.2-latest" that instead contain the githash.
                    first_file = tar_handle.getnames()[0]
                    tar_handle.extractall(path=temp_dir)
            else:
                raise Exception("Unsupported file extension {}".format(file_suffix))

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
        """Symlinks the binaries in the 'installed_dir' to the 'link_dir.'"""
        try:
            os.makedirs(self.link_dir)
        except OSError as exc:
            if exc.errno == errno.EEXIST and os.path.isdir(self.link_dir):
                pass
            else:
                raise

        for executable in os.listdir(os.path.join(installed_dir, "bin")):

            executable_name, executable_extension = os.path.splitext(executable)
            link_name = "{}-{}{}".format(executable_name, version, executable_extension)

            try:
                executable = os.path.join(installed_dir, "bin", executable)
                executable_link = os.path.join(self.link_dir, link_name)
                if os.name == "nt":
                    # os.symlink is not supported on Windows, use a direct method instead.
                    def symlink_ms(source, link_name):
                        """Provides symlink for Windows."""
                        import ctypes
                        csl = ctypes.windll.kernel32.CreateSymbolicLinkW
                        csl.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
                        csl.restype = ctypes.c_ubyte
                        flags = 1 if os.path.isdir(source) else 0
                        if csl(link_name, source.replace("/", "\\"), flags) == 0:
                            raise ctypes.WinError()
                    os.symlink = symlink_ms
                os.symlink(executable, executable_link)
            except OSError as exc:
                if exc.errno == errno.EEXIST:
                    pass
                else:
                    raise


def main():
    """Main program."""

    # Listen for SIGUSR1 and dump stack if received.
    try:
        signal.signal(signal.SIGUSR1, dump_stacks)
    except AttributeError:
        print("Cannot catch signals on Windows")

    parser = optparse.OptionParser(usage="""
Downloads and installs particular mongodb versions (each binary is renamed
to include its version) into an install directory and symlinks the binaries
with versions to another directory. This script supports community and
enterprise builds.

Usage: setup_multiversion_mongodb.py [options] ver1 [vers2 ...]

Ex: setup_multiversion_mongodb.py --installDir ./install
                                  --linkDir ./link
                                  --edition base
                                  --platformArchitecture Linux/x86_64 2.0.6 2.0.3-rc0
                                  2.0 2.2 2.3
Ex: setup_multiversion_mongodb.py --installDir ./install
                                  --linkDir ./link
                                  --edition enterprise
                                  --platformArchitecture osx
                                  2.4 2.2

After running the script you will have a directory structure like this:
    ./install/[mongodb-osx-x86_64-2.4.9, mongodb-osx-x86_64-2.2.7]
    ./link/[mongod-2.4.9, mongod-2.2.7, mongo-2.4.9...]

You should then add ./link/ to your path so multi-version tests will work.

Note: If "rc" is included in the version name, we'll use the exact rc, otherwise
we'll pull the highest non-rc version compatible with the version specified.
""")

    parser.add_option("-i", "--installDir",
                      dest="install_dir",
                      help="Directory to install the download archive. [REQUIRED]",
                      default=None)
    parser.add_option("-l", "--linkDir",
                      dest="link_dir",
                      help="Directory to contain links to all binaries for each version in"
                           " the install directory. [REQUIRED]",
                      default=None)
    editions = ["base", "enterprise", "targeted"]
    parser.add_option("-e", "--edition",
                      dest="edition",
                      choices=editions,
                      help="Edition of the build to download, choose from {}, [default:"
                           " '%default'].".format(editions),
                      default="base")
    parser.add_option("-p", "--platformArchitecture",
                      dest="platform_arch",
                      help="Platform/architecture to download. The architecture is not required."
                           "  [REQUIRED]. Examples include: 'linux/x86_64', 'osx', 'rhel62',"
                           " 'windows/x86_64-2008plus-ssl'.",
                      default=None)
    parser.add_option("-u", "--useLatest",
                      dest="use_latest",
                      action="store_true",
                      help="If specified, the latest (nightly) version will be downloaded,"
                           " if it exists, for the version specified. For example, if specifying"
                           " version 3.2 for download, the nightly version for 3.2 will be"
                           " downloaded if it exists, otherwise the 'highest' version will be"
                           " downloaded, i.e., '3.2.17'",
                      default=False)

    options, versions = parser.parse_args()

    # Check for required options.
    if (not versions or
        not options.install_dir or
        not options.link_dir or
        not options.platform_arch):
        parser.print_help()
        parser.exit(1)

    downloader = MultiVersionDownloader(
        options.install_dir,
        options.link_dir,
        options.edition,
        options.platform_arch,
        options.use_latest)

    for version in versions:
        downloader.download_version(version)


if __name__ == "__main__":
    main()
