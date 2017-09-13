"""
Testing hook for verifying members of a replica set have matching
oplogs.
"""

from __future__ import absolute_import

import os.path

from . import jsfile


class CheckReplOplogs(jsfile.JsCustomBehavior):
    """
    Checks that local.oplog.rs matches on the primary and secondaries.
    """
    def __init__(self, hook_logger, fixture, shell_options=None):
        description = "Check oplogs of all replica set members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_oplogs.js")
        jsfile.JsCustomBehavior.__init__(self,
                                         hook_logger,
                                         fixture,
                                         js_filename,
                                         description,
                                         shell_options=shell_options)
