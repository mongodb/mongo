#!/usr/bin/env python3
from typing import Dict, Optional

from buildscripts.resmokelib.extensions.constants import (
    TEST_PUBLIC_KEY_PATH,
)


def add_extensions_signature_pub_key_path(
    mongod_options: Dict,
    mongos_options: Optional[Dict] = None,
):
    EXTENSIONS_SIGNATURE_PUB_KEY_PATH_PARAM = "extensionsSignaturePublicKeyPath"
    mongod_options[EXTENSIONS_SIGNATURE_PUB_KEY_PATH_PARAM] = TEST_PUBLIC_KEY_PATH

    if mongos_options is not None:
        mongos_options[EXTENSIONS_SIGNATURE_PUB_KEY_PATH_PARAM] = TEST_PUBLIC_KEY_PATH
