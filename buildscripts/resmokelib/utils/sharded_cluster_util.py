import time
from uuid import uuid4

import bson
import pymongo.errors
from bson.binary import Binary, UuidRepresentation


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


def inject_catalog_metadata_on_the_csrs(csrs_client, inject_catalog_metadata):
    if inject_catalog_metadata["admin_db"]:
        add_admin_to_config_db(csrs_client)


def add_admin_to_config_db(csrs_client):
    """Adds garbage to the config database."""
    command_request = {
        "insert": "databases",
        "documents": [
            {
                "_id": "admin",
                "partitioned": False,
                "primary": "config",
                "version": {
                    "uuid": Binary.from_uuid(uuid4(), UuidRepresentation.STANDARD),
                    "lastMod": 1,
                    "timestamp": bson.Timestamp(1734606972, 123),
                },
            }
        ],
    }
    csrs_client.config.command(command_request)
