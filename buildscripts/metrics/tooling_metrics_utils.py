import atexit
import logging
import os
from typing import Any, Callable, Dict
import pymongo

logger = logging.getLogger('tooling_metrics')

INTERNAL_TOOLING_METRICS_HOSTNAME = "mongodb+srv://dev-metrics-pl-0.kewhj.mongodb.net"
INTERNAL_TOOLING_METRICS_USERNAME = "internal_tooling_user"
INTERNAL_TOOLING_METRICS_PASSWORD = "internal_tooling_user"


def _get_internal_tooling_metrics_client() -> pymongo.MongoClient:
    """Retrieve client for internal MongoDB tooling metrics cluster."""
    return pymongo.MongoClient(
        host=INTERNAL_TOOLING_METRICS_HOSTNAME,
        username=INTERNAL_TOOLING_METRICS_USERNAME,
        password=INTERNAL_TOOLING_METRICS_PASSWORD,
        socketTimeoutMS=1000,
        serverSelectionTimeoutMS=1000,
        connectTimeoutMS=1000,
        waitQueueTimeoutMS=1000,
        retryWrites=False,
    )


MONGOD_INTENRAL_DISTRO_FILEPATH = '/etc/mongodb-distro-name'


def _is_virtual_workstation() -> bool:
    """Detect whether this is a MongoDB internal virtual workstation."""
    try:
        with open(MONGOD_INTENRAL_DISTRO_FILEPATH, 'r') as file:
            return file.read().strip() == 'ubuntu1804-workstation'
    except Exception as _:  # pylint: disable=broad-except
        return False


TOOLING_METRICS_OPT_OUT = "TOOLING_METRICS_OPT_OUT"


def _has_metrics_opt_out() -> bool:
    """Check whether the opt out environment variable is set."""
    return os.environ.get(TOOLING_METRICS_OPT_OUT, None) == '1'


def _should_collect_metrics() -> bool:
    """Determine whether to collect tooling metrics."""
    return _is_virtual_workstation() and not _has_metrics_opt_out()


# DO NOT USE DIRECTLY -- This is only to be used when metrics collection is registered atexit
def _save_metrics(
        generate_metrics_function: Callable,
        generate_metrics_args: Dict[str, Any],
) -> None:
    """Save metrics to the atlas cluster."""
    try:
        client = _get_internal_tooling_metrics_client()
        metrics = generate_metrics_function(**generate_metrics_args)
        client.metrics.tooling_metrics.insert_one(metrics.dict())
    except Exception as exc:  # pylint: disable=broad-except
        logger.warning(
            "%s\n\nInternal Metrics Collection Failed -- this is a non-issue.\nIf this message persists, feel free to reach out to #server-dev-platform",
            exc)


# This is the only util that should be used externally
def register_metrics_collection_atexit(
        generate_metrics_function: Callable,
        generate_metrics_args: Dict[str, Any],
) -> None:
    """Register metrics collection on atexit."""
    if _should_collect_metrics():
        atexit.register(_save_metrics, generate_metrics_function, generate_metrics_args)
