"""
Set of utility helper functions to get information about a replica set.

These helpers can be used for any replica set, not only ones started by
resmoke.py.
"""

import bson

from buildscripts.resmokelib import errors


def get_last_optime(client):
    """Get the latest optime.

    This function is derived from _getLastOpTime() in ReplSetTest.
    """
    repl_set_status = client.admin.command({"replSetGetStatus": 1})
    conn_status = [m for m in repl_set_status["members"] if "self" in m][0]
    optime = conn_status["optime"]

    optime_is_empty = False

    if isinstance(optime, bson.Timestamp):  # PV0
        optime_is_empty = (optime == bson.Timestamp(0, 0))
    else:  # PV1
        optime_is_empty = (optime["ts"] == bson.Timestamp(0, 0) and optime["t"] == -1)

    if optime_is_empty:
        raise errors.ServerFailure(
            "Uninitialized opTime being reported by {addr[0]}:{addr[1]}: {repl_set_status}".format(
                addr=client.address, repl_set_status=repl_set_status))

    return optime
