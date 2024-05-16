"""Setup multiversion config."""

from typing import List

SETUP_MULTIVERSION_CONFIG = (
    "buildscripts/resmokeconfig/setup_multiversion/setup_multiversion_config.yml"
)

# Records the paths of installed multiversion binaries on Windows.
WINDOWS_BIN_PATHS_FILE = "windows_binary_paths.txt"


class Buildvariant:
    """Class represents buildvariant in setup multiversion config."""

    name: str
    edition: str
    platform: str
    architecture: str
    versions: List[str]

    def __init__(self, buildvariant_yaml: dict):
        """Initialize."""
        self.name = buildvariant_yaml.get("name", "")
        self.edition = buildvariant_yaml.get("edition", "")
        self.platform = buildvariant_yaml.get("platform", "")
        self.architecture = buildvariant_yaml.get("architecture", "")
        self.versions = buildvariant_yaml.get("versions", [])


class SetupMultiversionConfig:
    """Class represents setup multiversion config."""

    evergreen_projects: List[str]
    evergreen_buildvariants: List[Buildvariant]

    def __init__(self, raw_yaml: dict):
        """Initialize."""
        self.evergreen_projects = raw_yaml.get("evergreen_projects", [])
        self.evergreen_buildvariants = []
        buildvariants_raw_yaml = raw_yaml.get("evergreen_buildvariants", "")
        for buildvariant_yaml in buildvariants_raw_yaml:
            self.evergreen_buildvariants.append(Buildvariant(buildvariant_yaml))
