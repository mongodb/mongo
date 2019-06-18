"""Common code for mock kms http endpoint."""
import json

URL_PATH_STATS = "/stats"
URL_DISABLE_FAULTS = "/disable_faults"
URL_ENABLE_FAULTS = "/enable_faults"

class Stats:
    """Stats class shared between client and server."""

    def __init__(self):
        self.encrypt_calls = 0
        self.decrypt_calls = 0
        self.fault_calls = 0

    def __repr__(self):
        return json.dumps({
            'decrypts': self.decrypt_calls,
            'encrypts': self.encrypt_calls,
            'faults': self.fault_calls,
        })
