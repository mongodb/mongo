from datetime import datetime
import logging

from buildscripts.metrics.metrics_datatypes import ToolingMetrics
from buildscripts.metrics.tooling_metrics_utils import save_tooling_metrics, should_collect_metrics

logger = logging.getLogger('resmoke_tooling_metrics')


def save_resmoke_tooling_metrics(utc_starttime: datetime):
    try:
        if not should_collect_metrics():
            return
        tooling_metrics = ToolingMetrics.get_resmoke_metrics(utc_starttime)
        save_tooling_metrics(tooling_metrics)
    except Exception as exc:  # pylint: disable=broad-except
        logger.warning(
            "%s\nResmoke Metrics Collection Failed -- this is a non-issue.\nIf this message persists, feel free to reach out to #server-development-platform",
            exc)
