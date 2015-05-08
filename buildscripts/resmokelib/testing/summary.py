"""
Holder for summary information about a test group or suite.
"""

from __future__ import absolute_import

import collections



Summary = collections.namedtuple("Summary", ["num_run", "time_taken", "num_succeeded",
                                             "num_skipped", "num_failed", "num_errored"])


def combine(summary1, summary2):
    """
    Returns a summary representing the sum of 'summary1' and 'summary2'.
    """
    args = []
    for i in xrange(len(Summary._fields)):
        args.append(summary1[i] + summary2[i])
    return Summary._make(args)
