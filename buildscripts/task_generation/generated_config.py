"""Generated configuration."""
from typing import NamedTuple, List

from buildscripts.util.fileops import write_file_to_dir


class GeneratedFile(NamedTuple):
    """
    Generated configuration file.

    file_name: Name of generated configuration.
    content: Content of generated configuration.
    """

    file_name: str
    content: str

    def write_to_dir(self, directory: str) -> None:
        """
        Write this file to the given directory.

        :param directory: Directory to write file to.
        """
        write_file_to_dir(directory, self.file_name, self.content, overwrite=False)


class GeneratedConfiguration(NamedTuple):
    """
    Contain for the configuration needed to generate a task.

    file_list: List of filenames and file contents needed to generate a task.
    """

    file_list: List[GeneratedFile]

    def write_all_to_dir(self, directory: str) -> None:
        """
        Write all the configuration files to the given directory.

        :param directory: Directory to write to.
        """
        for item in self.file_list:
            item.write_to_dir(directory)
