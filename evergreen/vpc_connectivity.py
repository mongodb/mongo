import sys
import os

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.metrics.tooling_metrics import _get_internal_tooling_metrics_client

client = _get_internal_tooling_metrics_client()
print(client.server_info())
