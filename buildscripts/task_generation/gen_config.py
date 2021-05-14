"""Global configuration for generating tasks."""
from typing import Set

from pydantic import BaseModel

from buildscripts.util.fileops import read_yaml_file


class GenerationConfiguration(BaseModel):
    """Configuration for generating sub-tasks."""

    build_variant_large_distro_exceptions: Set[str]

    @classmethod
    def from_yaml_file(cls, path: str) -> "GenerationConfiguration":
        """Read the generation configuration from the given file."""
        return cls(**read_yaml_file(path))

    @classmethod
    def default_config(cls) -> "GenerationConfiguration":
        """Create a default configuration."""
        return cls(build_variant_large_distro_exceptions=set())
