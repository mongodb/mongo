import dataclasses
import datetime
import logging
import os
import asyncio
from typing import Optional
from git import Repo
import pymongo

from buildscripts.metrics.metrics_datatypes import ToolingMetrics

logger = logging.getLogger('tooling_metrics_collection')

INTERNAL_TOOLING_METRICS_HOSTNAME = "mongodb+srv://dev-metrics-pl-0.kewhj.mongodb.net"
INTERNAL_TOOLING_METRICS_USERNAME = "internal_tooling_user"
INTERNAL_TOOLING_METRICS_PASSWORD = "internal_tooling_user"


def _get_internal_tooling_metrics_client():
    """Retrieve client for internal MongoDB tooling metrics cluster."""
    return pymongo.MongoClient(
        host=INTERNAL_TOOLING_METRICS_HOSTNAME,
        username=INTERNAL_TOOLING_METRICS_USERNAME,
        password=INTERNAL_TOOLING_METRICS_PASSWORD,
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


async def _save_metrics(metrics: ToolingMetrics) -> None:
    """Save tooling metrics data."""
    client = _get_internal_tooling_metrics_client()
    client.metrics.tooling_metrics.insert_one(dataclasses.asdict(metrics))


def save_tooling_metrics(utc_starttime: datetime) -> None:
    """Persist tooling metrics data to MongoDB Internal Atlas Cluster."""
    try:
        if not _is_virtual_workstation():
            return
        asyncio.run(asyncio.wait_for(_save_metrics(ToolingMetrics(utc_starttime)), timeout=1.0))
    except asyncio.TimeoutError as exc:
        logger.warning(
            "%s\nTimeout: Tooling metrics collection is not available -- this is a non-issue.\nIf this message persists, feel free to reach out to #server-development-platform",
            exc)
    except Exception as exc:  # pylint: disable=broad-except
        logger.warning(
            "%s\nUnexpected: Tooling metrics collection is not available -- this is a non-issue.\nIf this message persists, feel free to reach out to #server-development-platform",
            exc)
