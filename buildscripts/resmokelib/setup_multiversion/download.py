"""Helper functions to download."""
import contextlib
import errno
import os
import shutil
import tarfile
import tempfile
import zipfile

import boto3
import structlog
from botocore import UNSIGNED
from botocore.config import Config
from botocore.exceptions import ClientError

S3_BUCKET = "mciuploads"

LOGGER = structlog.getLogger(__name__)


class DownloadError(Exception):
    """Errors in download.py."""

    pass


def download_mongodb(url):
    """Download file from S3 bucket by a given URL."""

    if not url:
        raise DownloadError("Download URL not found.")

    LOGGER.info("Downloading mongodb.", url=url)
    s3_key = url.split('/', 3)[-1].replace(f"{S3_BUCKET}/", "")
    filename = os.path.join(tempfile.gettempdir(), url.split('/')[-1])

    LOGGER.debug("Downloading mongodb from S3.", s3_bucket=S3_BUCKET, s3_key=s3_key,
                 filename=filename)
    s3_client = boto3.client("s3", config=Config(signature_version=UNSIGNED))
    try:
        s3_client.download_file(S3_BUCKET, s3_key, filename)
    except ClientError as s3_client_error:
        LOGGER.error("Download failed due to S3 client error.")
        raise s3_client_error
    except Exception as ex:  # pylint: disable=broad-except
        LOGGER.error("Download failed.")
        raise ex
    else:
        LOGGER.info("Download completed.", filename=filename)

    return filename


def extract_archive(archive_file, install_dir):
    """Uncompress file and return root of extracted directory."""

    LOGGER.info("Extracting archive data.", archive=archive_file, install_dir=install_dir)
    temp_dir = tempfile.mkdtemp()
    archive_name = os.path.basename(archive_file)
    install_subdir, file_suffix = os.path.splitext(archive_name)

    if file_suffix == ".zip":
        # Support .zip downloads, used for Windows binaries.
        with zipfile.ZipFile(archive_file) as zip_handle:
            first_file = zip_handle.namelist()[0]
            zip_handle.extractall(temp_dir)
    elif file_suffix == ".tgz":
        # Support .tgz downloads, used for Linux binaries.
        with contextlib.closing(tarfile.open(archive_file, "r:gz")) as tar_handle:
            first_file = tar_handle.getnames()[0]
            tar_handle.extractall(path=temp_dir)
    else:
        raise DownloadError(f"Unsupported file extension {file_suffix}")

    extracted_root_dir = os.path.join(temp_dir, os.path.dirname(first_file))
    temp_install_dir = tempfile.mkdtemp()
    shutil.move(extracted_root_dir, os.path.join(temp_install_dir, install_subdir))

    try:
        os.makedirs(install_dir)
    except OSError as exc:
        if exc.errno == errno.EEXIST and os.path.isdir(install_dir):
            pass
        else:
            raise

    already_downloaded = os.path.isdir(os.path.join(install_dir, install_subdir))
    if not already_downloaded:
        shutil.move(os.path.join(temp_install_dir, install_subdir), install_dir)

    shutil.rmtree(temp_dir)
    shutil.rmtree(temp_install_dir)

    installed_dir = os.path.join(install_dir, install_subdir)
    LOGGER.info("Extract archive completed.", installed_dir=installed_dir)

    return installed_dir


def symlink_version(version, installed_dir, link_dir):
    """Symlink the binaries in the 'installed_dir' to the 'link_dir'."""
    try:
        os.makedirs(link_dir)
    except OSError as exc:
        if exc.errno == errno.EEXIST and os.path.isdir(link_dir):
            pass
        else:
            raise

    for executable in os.listdir(os.path.join(installed_dir, "bin")):

        executable_name, executable_extension = os.path.splitext(executable)
        link_name = f"{executable_name}-{version}{executable_extension}"

        try:
            executable = os.path.join(installed_dir, "bin", executable)
            executable_link = os.path.join(link_dir, link_name)

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

                os.symlink = symlink_ms
            os.symlink(executable, executable_link)
            LOGGER.debug("Symlink created.", executable=executable, executable_link=executable_link)

        except OSError as exc:
            if exc.errno == errno.EEXIST:
                pass
            else:
                raise

    LOGGER.info("Symlinks for all executables are created in the directory.", link_dir=link_dir)
