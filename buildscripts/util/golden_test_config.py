"""Shared helpers for the golden data test framework configuration.

Both ``buildscripts/golden_test.py`` (the user-facing diff/accept CLI) and
``buildscripts/resmokelib/run/__init__.py`` (resmoke.py's ``run`` subcommand)
need to read the YAML config pointed to by the ``GOLDEN_TEST_CONFIG_PATH``
environment variable. This module centralises that handling.

For details on the framework see ``docs/golden_data_test_framework.md``.
"""

import os
from dataclasses import dataclass
from typing import Optional

from buildscripts.util.fileops import read_yaml_file

GOLDEN_TEST_CONFIG_PATH_ENV = "GOLDEN_TEST_CONFIG_PATH"
GOLDEN_TEST_OUTPUT_ROOT_PATTERN_ENV = "GOLDEN_TEST_OUTPUT_ROOT_PATTERN"


@dataclass
class GoldenTestConfig:
    """Parsed contents of a golden test YAML config file."""

    outputRootPattern: Optional[str] = None
    diffCmd: Optional[str] = None

    @classmethod
    def from_yaml_file(cls, path: str) -> "GoldenTestConfig":
        """Read the golden test configuration from a YAML file."""
        data = read_yaml_file(path) or {}
        return cls(
            outputRootPattern=data.get("outputRootPattern"),
            diffCmd=data.get("diffCmd"),
        )

    @staticmethod
    def default_config_path() -> str:
        """Return the default path for the golden test configuration file."""
        return os.path.join(os.path.expanduser("~"), ".golden_test_config.yml")
