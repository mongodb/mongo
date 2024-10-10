import datetime

import mongo_tooling_metrics.client as metrics_client
import pkg_resources
import pymongo
from mongo_tooling_metrics.lib.top_level_metrics import (
    NinjaToolingMetrics,
    ResmokeToolingMetrics,
    SConsToolingMetrics,
)
from pydantic import ValidationError

# Check cluster connectivity
try:
    client = pymongo.MongoClient(
        host=metrics_client.INTERNAL_TOOLING_METRICS_HOSTNAME,
        username=metrics_client.INTERNAL_TOOLING_METRICS_USERNAME,
        password=metrics_client.INTERNAL_TOOLING_METRICS_PASSWORD,
    )
    client.server_info()
except Exception as exc:
    print("Could not connect to Atlas cluster")
    raise exc

metrics_classes = {
    "ninja": NinjaToolingMetrics,
    "scons": SConsToolingMetrics,
    "resmoke": ResmokeToolingMetrics,
}


def get_metrics_data(source, lookback=30):
    try:
        # Get SCons metrics for the lookback period
        tooling_metrics_version = pkg_resources.get_distribution("mongo-tooling-metrics").version
        lookback_datetime = datetime.datetime.utcnow() - datetime.timedelta(days=lookback)
        last_week_metrics = client.metrics.tooling_metrics.find(
            {
                "source": source,
                "utc_starttime": {"$gt": lookback_datetime},
                "tooling_metrics_version": tooling_metrics_version,
            }
        )

        malformed_metrics = []
        invalid_metrics = []
        total_docs = 0

        # Find any malformed/invalid documents in the cluster
        for doc in last_week_metrics:
            total_docs += 1
            try:
                metrics = metrics_classes[source](**doc)
                if metrics.is_malformed():
                    malformed_metrics.append(doc["_id"])
            except ValidationError:
                invalid_metrics.append(doc["_id"])

        metrics_detailed = (
            f"METRICS DETAILED ({source}):\n"
            f"malformed_metrics_last_week: {malformed_metrics}\n"
            f"invalid_metrics_last_week: {invalid_metrics}\n"
            f"total_docs_last_week: {total_docs}\n"
            f"tooling_metrics_version: {tooling_metrics_version}\n"
        )
        metrics_overview = (
            f"METRICS OVERVIEW ({source}):\n"
            f"malformed_metrics_last_week: {len(malformed_metrics)} ({len(malformed_metrics)/total_docs*100:.2f}%)\n"
            f"invalid_metrics_last_week: {len(invalid_metrics)} ({len(invalid_metrics)/total_docs*100:.2f}%)\n"
            f"total_docs_last_week: {total_docs}\n"
            f"tooling_metrics_version: {tooling_metrics_version}\n"
        )

        print(metrics_overview)
        print(metrics_detailed)

        return metrics_overview

    except Exception as exc:
        print("Unexpected failure while getting metrics")
        raise exc


ninja_metrics_overview = get_metrics_data("ninja")
scons_metrics_overview = get_metrics_data("scons")
resmoke_metrics_overview = get_metrics_data("resmoke")
