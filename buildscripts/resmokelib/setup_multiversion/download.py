"""Helper functions to download."""

import contextlib
import errno
import glob
import os
import shutil
import tarfile
import zipfile

import requests
import structlog

from buildscripts.resmokelib.utils.filesystem import build_hygienic_bin_path, mkdtemp_in_build_dir

S3_BUCKET = "mciuploads"

LOGGER = structlog.getLogger(__name__)


class DownloadError(Exception):
    """Errors in download.py."""

    pass


def download_from_s3(url):
    """Download file from S3 bucket by a given URL."""

    if not url:
        raise DownloadError("Download URL not found")

    LOGGER.info("Downloading.", url=url)
    filename = os.path.join(mkdtemp_in_build_dir(), url.split("/")[-1].split("?")[0])

    with requests.get(url, stream=True) as reader:
        with open(filename, "wb") as file_handle:
            shutil.copyfileobj(reader.raw, file_handle)

    return filename


def _rsync_move_dir(source_dir, dest_dir):
    """
    Move dir.

    Move the contents of `source_dir` into `dest_dir` as a subdir while merging with
    all existing dirs.

    This is similar to the behavior of `rsync` but different to `mv`.
    """

    for cur_src_dir, _, files in os.walk(source_dir):
        cur_dest_dir = cur_src_dir.replace(source_dir, dest_dir, 1)
        if not os.path.exists(cur_dest_dir):
            os.makedirs(cur_dest_dir)
        for cur_file in files:
            src_file = os.path.join(cur_src_dir, cur_file)
            dst_file = os.path.join(cur_dest_dir, cur_file)
            if os.path.exists(dst_file):
                # in case of the src and dst are the same file
                if os.path.samefile(src_file, dst_file):
                    continue
                os.remove(dst_file)
            shutil.move(src_file, cur_dest_dir)


def extract_archive(archive_file, install_dir):
    """Uncompress file and return root of extracted directory."""

    LOGGER.info("Extracting archive data.", archive=archive_file, install_dir=install_dir)
    temp_dir = mkdtemp_in_build_dir()
    archive_name = os.path.basename(archive_file)
    _, file_suffix = os.path.splitext(archive_name)

    if file_suffix == ".zip":
        # Support .zip downloads, used for Windows binaries.
        with zipfile.ZipFile(archive_file) as zip_handle:
            zip_handle.extractall(temp_dir)
    elif file_suffix == ".tgz":
        # Support .tgz downloads, used for Linux binaries.
        with contextlib.closing(tarfile.open(archive_file, "r:gz")) as tar_handle:
            tar_handle.extractall(path=temp_dir)
    else:
        raise DownloadError(f"Unsupported file extension {file_suffix}")

    # Pre-hygienic tarballs have a unique top-level dir when untarred. We ignore
    # that dir to ensure the untarred dir structure is uniform. symbols and artifacts
    # are rarely used on pre-hygienic versions so we ignore them for simplicity.
    bin_archive_root = glob.glob(os.path.join(temp_dir, "mongodb-*", "bin"))
    if bin_archive_root:
        temp_dir = bin_archive_root[0]

    try:
        os.makedirs(install_dir)
    except FileExistsError:
        pass

    _rsync_move_dir(temp_dir, install_dir)
    shutil.rmtree(temp_dir)

    LOGGER.info("Extract archive completed.", installed_dir=install_dir)

    return install_dir


def mkdir_p(path):
    """Python equivalent of `mkdir -p`."""
    try:
        os.makedirs(path)
    except OSError as exc:
        if exc.errno == errno.EEXIST and os.path.isdir(path):
            pass
        else:
            raise


def symlink_version(suffix, installed_dir, link_dir=None):
    """
    Symlink the binaries in the 'installed_dir' to the 'link_dir'.

    If `link_dir` is None, link to the physical executable's directory (`bin_dir`).
    """
    hygienic_bin_dir = build_hygienic_bin_path(parent=installed_dir)
    if os.path.isdir(hygienic_bin_dir):
        bin_dir = hygienic_bin_dir
    else:
        bin_dir = installed_dir

    if link_dir is None:
        link_dir = bin_dir
    else:
        mkdir_p(link_dir)

    for executable in os.listdir(bin_dir):
        if executable.endswith(".dll"):
            LOGGER.debug("Skipping linking DLL", file=executable)
            continue

        executable_name, executable_extension = os.path.splitext(executable)
        if suffix:
            link_name = f"{executable_name}-{suffix}{executable_extension}"
        else:
            link_name = executable

        try:
            executable = os.path.join(bin_dir, executable)
            executable_link = os.path.join(link_dir, link_name)

            link_method = os.symlink
            if os.name == "nt":
                # os.symlink is not supported on Windows, use a direct method instead.
                def symlink_ms(source, symlink_name):
                    """Provide symlink for Windows."""
                    import ctypes

                    csl = ctypes.windll.kernel32.CreateSymbolicLinkW
                    csl.argtypes = (ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32)
                    csl.restype = ctypes.c_ubyte
                    flags = 1 if os.path.isdir(source) else 0
                    if csl(symlink_name, source.replace("/", "\\"), flags) == 0:
                        raise ctypes.WinError()

                link_method = symlink_ms
            link_method(executable, executable_link)
            LOGGER.debug("Symlink created.", executable=executable, executable_link=executable_link)

        except OSError as exc:
            if exc.errno == errno.EEXIST:
                pass
            else:
                raise

    LOGGER.info("Symlinks for all executables are created in the directory.", link_dir=link_dir)
    return link_dir
