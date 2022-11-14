import logging
import os
import asyncio
from typing import Optional
from git import Repo
import pymongo

from buildscripts.metrics.metrics_datatypes import ToolingMetrics

logger = logging.getLogger('tooling_metrics_utils')

INTERNAL_TOOLING_METRICS_HOSTNAME = "mongodb+srv://dev-metrics-pl-0.kewhj.mongodb.net"
INTERNAL_TOOLING_METRICS_USERNAME = "internal_tooling_user"
INTERNAL_TOOLING_METRICS_PASSWORD = "internal_tooling_user"


def _get_internal_tooling_metrics_client():
    """Retrieve client for internal MongoDB tooling metrics cluster."""
    return pymongo.MongoClient(
        host=INTERNAL_TOOLING_METRICS_HOSTNAME,
        username=INTERNAL_TOOLING_METRICS_USERNAME,
        password=INTERNAL_TOOLING_METRICS_PASSWORD,
        socketTimeoutMS=1000,
        serverSelectionTimeoutMS=1000,
        connectTimeoutMS=1000,
        waitQueueTimeoutMS=1000,
    )


EXPECTED_TOOLCHAIN_LOCATION = "/opt/mongodbtoolchain"


def _toolchain_exists() -> bool:
    """Check if the internal MongoDB toolchain exists."""
    return os.path.exists(EXPECTED_TOOLCHAIN_LOCATION)


def _git_user_exists() -> Optional[str]:
    """Check if a git user email exists."""
    try:
        return Repo('.').config_reader().get_value("user", "email", None)
    except Exception:  # pylint: disable=broad-except
        return None


def _is_virtual_workstation() -> bool:
    """Detect whether this is a MongoDB internal virtual workstation."""
    return _toolchain_exists() and _git_user_exists()


TOOLING_METRICS_OPT_OUT = "TOOLING_METRICS_OPT_OUT"


def _has_metrics_opt_out() -> bool:
    """Check whether the opt out environment variable is set."""
    return os.environ.get(TOOLING_METRICS_OPT_OUT, None) == '1'


def should_collect_metrics() -> bool:
    """Determine whether to collect tooling metrics."""
    return _is_virtual_workstation() and not _has_metrics_opt_out()


async def _save_metrics(metrics: ToolingMetrics) -> None:
    """Save tooling metrics data."""
    client = _get_internal_tooling_metrics_client()
    client.metrics.tooling_metrics.insert_one(metrics.dict())


def save_tooling_metrics(tooling_metrics: ToolingMetrics) -> None:
    """Persist tooling metrics data to MongoDB Internal Atlas Cluster."""
    try:
        asyncio.run(asyncio.wait_for(_save_metrics(tooling_metrics), timeout=1.0))
    except asyncio.TimeoutError as exc:
        logger.warning(
            "%s\nTimeout: Tooling metrics collection is not available -- this is a non-issue.\nIf this message persists, feel free to reach out to #server-development-platform",
            exc)
    except Exception as exc:  # pylint: disable=broad-except
        logger.warning(
            "%s\nUnexpected: Tooling metrics collection is not available -- this is a non-issue.\nIf this message persists, feel free to reach out to #server-development-platform",
            exc)
