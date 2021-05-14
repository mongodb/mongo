"""Utilities for generating with multiversion tests."""
from typing import List

import inject

from buildscripts.task_generation.resmoke_proxy import ResmokeProxyService

REPL_MIXED_VERSION_CONFIGS = ["new-old-new", "new-new-old", "old-new-new"]
SHARDED_MIXED_VERSION_CONFIGS = ["new-old-old-new"]


class MultiversionUtilService:
    """Utilities to working with multiversion tests."""

    @inject.autoparams()
    def __init__(self, resmoke_proxy: ResmokeProxyService) -> None:
        """
        Initialize the service.

        :param resmoke_proxy: Resmoke proxy service.
        """
        self.resmoke_proxy = resmoke_proxy

    def is_suite_sharded(self, suite_name: str) -> bool:
        """Return true if a suite uses ShardedClusterFixture."""
        source_config = self.resmoke_proxy.read_suite_config(suite_name)
        return source_config["executor"]["fixture"]["class"] == "ShardedClusterFixture"

    def get_version_configs_for_suite(self, suite_name: str) -> List[str]:
        """
        Get the version configs that apply for the given suite.

        :param suite_name: Suite to get version configs for.
        :return: List of version configs.
        """
        is_sharded = self.is_suite_sharded(suite_name)
        return self.get_version_configs(is_sharded)

    @staticmethod
    def get_version_configs(is_sharded: bool) -> List[str]:
        """Get the version configurations to use."""
        if is_sharded:
            return SHARDED_MIXED_VERSION_CONFIGS
        return REPL_MIXED_VERSION_CONFIGS
