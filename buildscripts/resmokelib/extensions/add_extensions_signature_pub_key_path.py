#!/usr/bin/env python3
from typing import Dict, Optional

from buildscripts.resmokelib.extensions.constants import (
    TEST_PUBLIC_KEY_PATH,
)


def add_extensions_signature_pub_key_path(
    skip_extensions_signature_verification,
    config,
    mongod_options: Dict,
    mongos_options: Optional[Dict] = None,
):
    # We omit providing the extension signature public key path parameter if we intend to skip signature verification.
    # This signals to the server in insecure mode to skip validating extension signatures at load time.
    if skip_extensions_signature_verification or config.SKIP_EXTENSIONS_SIGNATURE_VERIFICATION:
        return

    EXTENSIONS_SIGNATURE_PUB_KEY_PATH_PARAM = "extensionsSignaturePublicKeyPath"
    mongod_options[EXTENSIONS_SIGNATURE_PUB_KEY_PATH_PARAM] = TEST_PUBLIC_KEY_PATH

    if mongos_options is not None:
        mongos_options[EXTENSIONS_SIGNATURE_PUB_KEY_PATH_PARAM] = TEST_PUBLIC_KEY_PATH
