import time

import pymongo.errors


def refresh_logical_session_cache_with_retry(mongo_client, csrs=None):
    retry_count = 10
    while retry_count > 0:
        try:
            mongo_client.admin.command({"refreshLogicalSessionCacheNow": 1})
            if csrs:
                # Ensure all CSRS nodes have advanced their majority commit time to include the
                # shardCollection for config.system.sessions namespace. This ensures subsequent
                # refreshes of this collection will see the sharded version despite the use of
                # direct connections. Only do this if we are using a config server connection.
                csrs.await_last_op_committed()
            break
        except pymongo.errors.OperationFailure as err:
            if err.code == 70:  # ShardNotFound
                time.sleep(0.5)  # Wait a little bit before trying again.
                retry_count -= 1
            raise err
    if retry_count == 0:
        raise Exception("Unable to refresh the logical session cache for the config server.")
