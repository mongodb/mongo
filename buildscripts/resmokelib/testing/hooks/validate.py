"""Test hook for verifying the consistency and integrity of collection and index data."""

import os.path

from . import jsfile


class ValidateCollections(jsfile.DataConsistencyHook):
    """Run full validation.

    This will run on all collections in all databases on every stand-alone
    node, primary replica-set node, or primary shard node.
    """

    def __init__(  # pylint: disable=super-init-not-called
            self, hook_logger, fixture, shell_options=None):
        """Initialize ValidateCollections."""
        description = "Full collection validation"
        js_filename = os.path.join("jstests", "hooks", "run_validate_collections.js")
        jsfile.JSHook.__init__(  # pylint: disable=non-parent-init-called
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options)
