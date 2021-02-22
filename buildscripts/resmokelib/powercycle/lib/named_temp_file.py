"""Wrapper for the NamedTempFile class."""
import logging
import os
import shutil
import tempfile

LOGGER = logging.getLogger(__name__)


class NamedTempFile(object):
    """Class to control temporary files."""

    _FILE_MAP = {}  # type: ignore
    _DIR_LIST = []  # type: ignore

    @classmethod
    def create(cls, newline=None, suffix="", directory=None):
        """Create a temporary file, and optional directory, and returns the file name."""
        if directory and not os.path.isdir(directory):
            LOGGER.debug("Creating temporary directory %s", directory)
            os.makedirs(directory)
            cls._DIR_LIST.append(directory)
        temp_file = tempfile.NamedTemporaryFile(mode="w+", newline=newline, suffix=suffix,
                                                dir=directory, delete=False)
        cls._FILE_MAP[temp_file.name] = temp_file
        return temp_file.name

    @classmethod
    def get(cls, name):
        """Get temporary file object.  Raises an exception if the file is unknown."""
        if name not in cls._FILE_MAP:
            raise Exception("Unknown temporary file {}.".format(name))
        return cls._FILE_MAP[name]

    @classmethod
    def delete(cls, name):
        """Delete temporary file. Raises an exception if the file is unknown."""
        if name not in cls._FILE_MAP:
            raise Exception("Unknown temporary file {}.".format(name))
        if not os.path.exists(name):
            LOGGER.debug("Temporary file %s no longer exists", name)
            del cls._FILE_MAP[name]
            return
        try:
            os.remove(name)
        except (IOError, OSError) as err:
            LOGGER.warning("Unable to delete temporary file %s with error %s", name, err)
        if not os.path.exists(name):
            del cls._FILE_MAP[name]

    @classmethod
    def delete_dir(cls, directory):
        """Delete temporary directory. Raises an exception if the directory is unknown."""
        if directory not in cls._DIR_LIST:
            raise Exception("Unknown temporary directory {}.".format(directory))
        if not os.path.exists(directory):
            LOGGER.debug("Temporary directory %s no longer exists", directory)
            cls._DIR_LIST.remove(directory)
            return
        try:
            shutil.rmtree(directory)
        except (IOError, OSError) as err:
            LOGGER.warning("Unable to delete temporary directory %s with error %s", directory, err)
        if not os.path.exists(directory):
            cls._DIR_LIST.remove(directory)

    @classmethod
    def delete_all(cls):
        """Delete all temporary files and directories."""
        for name in list(cls._FILE_MAP):
            cls.delete(name)
        for directory in cls._DIR_LIST:
            cls.delete_dir(directory)
