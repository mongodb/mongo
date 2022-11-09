import datetime
import subprocess
import sys
import os

from pydantic import ValidationError

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.metrics.metrics_datatypes import ToolingMetrics
from buildscripts.metrics.tooling_metrics import _get_internal_tooling_metrics_client
from evergreen.api import RetryingEvergreenApi

# Check cluster connectivity
try:
    client = _get_internal_tooling_metrics_client()
    print(client.server_info())
except Exception as exc:
    print("Could not connect to Atlas cluster")
    raise exc

try:
    # Get metrics for the last week
    one_week_ago_datetime = datetime.datetime.utcnow() - datetime.timedelta(days=7)
    last_week_metrics = client.metrics.tooling_metrics.find(
        {"utc_starttime": {"$gt": one_week_ago_datetime}})

    malformed_metrics = []
    invalid_metrics = []
    total_docs = 0

    # Find any malformed/invalid documents in the cluster
    for doc in last_week_metrics:
        total_docs += 1
        try:
            metrics = ToolingMetrics(**doc)
            if metrics.is_malformed():
                malformed_metrics.append(doc['_id'])
        except ValidationError:
            invalid_metrics.append(doc['_id'])

    metrics_detailed = ("METRICS DETAILED:\n"
                        f"malformed_metrics_last_week: {malformed_metrics}\n"
                        f"invalid_metrics_last_week: {invalid_metrics}\n"
                        f"total_docs_last_week: {total_docs}")
    metrics_overview = (
        "METRICS OVERVIEW:\n"
        f"malformed_metrics_last_week: {len(malformed_metrics)} ({len(malformed_metrics)/total_docs*100:.2f}%)\n"
        f"invalid_metrics_last_week: {len(invalid_metrics)} ({len(invalid_metrics)/total_docs*100:.2f}%)\n"
        f"total_docs_last_week: {total_docs}")

    print(metrics_overview)
    print(metrics_detailed)

    # Publish metrics to SDP Slack Channel
    evg_api = RetryingEvergreenApi.get_api(config_file="./.evergreen.yml")
    evg_api.send_slack_message(target="#server-sdp-bfs", msg=metrics_overview)

except Exception as exc:
    print("Unexpected failure while getting metrics")
    raise exc
