import pymongo.errors


def refresh_logical_session_cache_with_retry(mongo_client):
    retry_count = 10
    while retry_count > 0:
        try:
            mongo_client.admin.command({"refreshLogicalSessionCacheNow": 1})
            break
        except pymongo.errors.OperationFailure as err:
            if err.code == 70:  # ShardNotFound
                time.sleep(0.5)  # Wait a little bit before trying again.
                retry_count -= 1
            raise err
    if retry_count == 0:
        raise Exception("Unable to refresh the logical session cache for the config server.")
