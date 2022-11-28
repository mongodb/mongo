import datetime
import sys
import os

from pydantic import ValidationError

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.metrics.metrics_datatypes import ResmokeToolingMetrics, SConsToolingMetrics
from buildscripts.metrics.tooling_metrics_utils import _get_internal_tooling_metrics_client
from evergreen.api import RetryingEvergreenApi

# Check cluster connectivity
try:
    client = _get_internal_tooling_metrics_client()
    print(client.server_info())
except Exception as exc:
    print("Could not connect to Atlas cluster")
    raise exc


def get_metrics_data(source, MetricsClass, lookback=7):
    try:
        # Get SCons metrics for the lookback period
        lookback_datetime = datetime.datetime.utcnow() - datetime.timedelta(days=lookback)
        last_week_metrics = client.metrics.tooling_metrics.find({
            "source": source,
            "utc_starttime": {"$gt": lookback_datetime},
        })

        malformed_metrics = []
        invalid_metrics = []
        total_docs = 0

        # Find any malformed/invalid documents in the cluster
        for doc in last_week_metrics:
            total_docs += 1
            try:
                metrics = MetricsClass(**doc)
                if metrics.is_malformed():
                    malformed_metrics.append(doc['_id'])
            except ValidationError:
                invalid_metrics.append(doc['_id'])

        metrics_detailed = (f"METRICS DETAILED ({source}):\n"
                            f"malformed_metrics_last_week: {malformed_metrics}\n"
                            f"invalid_metrics_last_week: {invalid_metrics}\n"
                            f"total_docs_last_week: {total_docs}\n")
        metrics_overview = (
            f"METRICS OVERVIEW ({source}):\n"
            f"malformed_metrics_last_week: {len(malformed_metrics)} ({len(malformed_metrics)/total_docs*100:.2f}%)\n"
            f"invalid_metrics_last_week: {len(invalid_metrics)} ({len(invalid_metrics)/total_docs*100:.2f}%)\n"
            f"total_docs_last_week: {total_docs}\n")

        print(metrics_overview)
        print(metrics_detailed)

        return metrics_overview

    except Exception as exc:
        print("Unexpected failure while getting metrics")
        raise exc


scons_metrics_overview = get_metrics_data("scons", SConsToolingMetrics)
resmoke_metrics_overview = get_metrics_data("resmoke", ResmokeToolingMetrics)

# Publish metrics to SDP Slack Channel
evg_api = RetryingEvergreenApi.get_api(config_file="./.evergreen.yml")
evg_api.send_slack_message(
    target="#server-sdp-bfs",
    msg=scons_metrics_overview + resmoke_metrics_overview,
)
