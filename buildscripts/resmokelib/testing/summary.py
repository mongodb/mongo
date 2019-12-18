"""Holder for summary information about a test suite."""

import collections

Summary = collections.namedtuple(
    "Summary",
    ["num_run", "time_taken", "num_succeeded", "num_skipped", "num_failed", "num_errored"])


def combine(summary1, summary2):
    """Return a summary representing the sum of 'summary1' and 'summary2'."""
    args = []
    for i in range(len(Summary._fields)):
        args.append(summary1[i] + summary2[i])
    return Summary._make(args)
