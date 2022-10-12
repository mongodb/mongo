import dataclasses
from datetime import datetime
import logging
from os.path import exists
import socket
import asyncio
from git import Repo
import pymongo

logger = logging.getLogger('tooling_metrics_collection')


@dataclasses.dataclass
class ToolingMetrics:
    """Class to store tooling metrics."""

    command: str
    utc_starttime: datetime
    utc_endtime: datetime
    ip_address: str = socket.gethostbyname(socket.gethostname())


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
    return exists(EXPECTED_TOOLCHAIN_LOCATION)


def _git_user_exists() -> bool:
    """Check if a git user email exists."""
    return Repo('.').config_reader().get_value("user", "email", None)


def _is_virtual_workstation() -> bool:
    """Detect whether this is a MongoDB internal virtual workstation."""
    return _toolchain_exists() and _git_user_exists()


async def _save_metrics(metrics: ToolingMetrics) -> None:
    """Save tooling metrics data."""
    client = _get_internal_tooling_metrics_client()
    client.metrics.tooling_metrics.insert_one(dataclasses.asdict(metrics))


def save_tooling_metrics(metrics: ToolingMetrics) -> None:
    """Persist tooling metrics data to MongoDB Internal Atlas Cluster."""
    if not _is_virtual_workstation():
        return
    try:
        asyncio.run(asyncio.wait_for(_save_metrics(metrics), timeout=1.0))
    except asyncio.TimeoutError as exc:
        logger.warning(
            "%s\nTimeout: Could not save tooling metrics data to MongoDB Atlas Cluster.\nIf this message persists, please reach out to #server-development-platform",
            exc)
    except Exception as exc:  # pylint: disable=broad-except
        logger.warning(
            "%s\nUnexpected: Could not save tooling metrics data to MongoDB Atlas Cluster.\nIf this message persists, please reach out to #server-development-platform",
            exc)
