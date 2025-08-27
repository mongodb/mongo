from uuid import uuid4

import bson
from bson.binary import Binary, UuidRepresentation


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
