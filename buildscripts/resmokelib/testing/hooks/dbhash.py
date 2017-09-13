"""
Testing hook for verifying data consistency across a replica set.
"""

from __future__ import absolute_import

import os.path

from . import jsfile


class CheckReplDBHash(jsfile.JsCustomBehavior):
    """
    Checks that the dbhashes of all non-local databases and non-replicated system collections
    match on the primary and secondaries.
    """
    def __init__(self, hook_logger, fixture, shell_options=None):
        description = "Check dbhashes of all replica set or master/slave members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_dbhash.js")
        jsfile.JsCustomBehavior.__init__(self,
                                         hook_logger,
                                         fixture,
                                         js_filename,
                                         description,
                                         shell_options=shell_options)
