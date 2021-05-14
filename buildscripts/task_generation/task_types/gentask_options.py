"""Options for generating evergreen tasks."""
import os
from typing import NamedTuple, Optional, List


class GenTaskOptions(NamedTuple):
    """
    Options for how Evergreen tasks should be generated.

    large_distro_name: Name of distro "large" tasks should be run on.
    create_misc_suite: Should "misc" suites be generated.
    is_patch: This generation is part of a patch build.
    generated_config_dir: Path to directory that configuration files should be written.
    use_default_timeouts: Don't overwrite task timeouts.
    """

    create_misc_suite: bool
    is_patch: bool
    generated_config_dir: str
    use_default_timeouts: bool

    def suite_location(self, suite_name: str) -> str:
        """
        Get the path to the given resmoke suite configuration file.

        :param suite_name: Name of resmoke suite to query.
        :return: Path to given resmoke suite.
        """
        return self.generated_file_location(os.path.basename(suite_name))

    def generated_file_location(self, base_file: str) -> str:
        """
        Get the path to the given base file.

        :param base_file: Base file to find.
        :return: Path to the given file.
        """
        # Evergreen always uses a unix shell, even on Windows, so instead of using os.path.join
        # here, just use the forward slash; otherwise the path separator will be treated as
        # the escape character on Windows.
        return "/".join([self.generated_config_dir, base_file])
