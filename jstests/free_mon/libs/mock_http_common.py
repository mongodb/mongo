"""Common code for mock free monitoring http endpoint."""
import json

URL_PATH_STATS = "/stats"
URL_PATH_LAST_REGISTER = "/last_register"
URL_PATH_LAST_METRICS = "/last_metrics"
URL_DISABLE_FAULTS = "/disable_faults"
URL_ENABLE_FAULTS = "/enable_faults"

class Stats:
    """Stats class shared between client and server."""

    def __init__(self):
        self.register_calls = 0
        self.metrics_calls = 0
        self.fault_calls = 0

    def __repr__(self):
        return json.dumps({
            'metrics': self.metrics_calls,
            'registers': self.register_calls,
            'faults': self.fault_calls,
        })
