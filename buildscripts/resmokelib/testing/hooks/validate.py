"""
Testing hook for verifying the consistency and integrity of collection
and index data.
"""

from __future__ import absolute_import

import os.path

from . import jsfile


class ValidateCollections(jsfile.JsCustomBehavior):
    """
    Runs full validation on all collections in all databases on every stand-alone
    node, primary replica-set node, or primary shard node.
    """
    def __init__(self, hook_logger, fixture, shell_options=None):
        description = "Full collection validation"
        js_filename = os.path.join("jstests", "hooks", "run_validate_collections.js")
        jsfile.JsCustomBehavior.__init__(self,
                                         hook_logger,
                                         fixture,
                                         js_filename,
                                         description,
                                         shell_options=shell_options)
