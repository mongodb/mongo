import atexit
import datetime
import logging
import sys
from typing import List

from buildscripts.metrics.metrics_datatypes import ToolingMetrics
from buildscripts.metrics.tooling_metrics_utils import is_virtual_workstation, save_tooling_metrics

logger = logging.getLogger('scons_tooling_metrics')


class SConsExitHook(object):
    """Plumb all sys.exit through this object so that we can access the exit code in atexit."""

    def __init__(self):
        self.exit_code = None
        self._orig_exit = sys.exit

    def __del__(self):
        sys.exit = self._orig_exit

    def initialize(self):
        sys.exit = self.exit

    def exit(self, code=0):
        self.exit_code = code
        self._orig_exit(code)


# This method should only be used when registered on atexit
def _save_scons_tooling_metrics(
        utc_starttime: datetime,
        env_vars: "SCons.Variables.Variables",
        env: "SCons.Script.SConscript.SConsEnvironment",
        parser: "SCons.Script.SConsOptions.SConsOptionParser",
        args: List[str],
        exit_hook: SConsExitHook,
):
    """Save SCons tooling metrics to atlas cluster."""
    try:
        if not is_virtual_workstation():
            return
        tooling_metrics = ToolingMetrics.get_scons_metrics(utc_starttime, env_vars, env, parser,
                                                           args, exit_hook.exit_code)
        save_tooling_metrics(tooling_metrics)
    except Exception as exc:  # pylint: disable=broad-except
        logger.warning(
            "%sSCons Metrics Collection Failed -- this is a non-issue.\nIf this message persists, feel free to reach out to #server-development-platform",
            exc)


def setup_scons_metrics_collection_atexit(
        utc_starttime: datetime,
        env_vars: "SCons.Variables.Variables",
        env: "SCons.Script.SConscript.SConsEnvironment",
        parser: "SCons.Script.SConsOptions.SConsOptionParser",
        args: List[str],
) -> None:
    """Register an atexit method for scons metrics collection."""
    scons_exit_hook = SConsExitHook()
    scons_exit_hook.initialize()
    atexit.register(
        _save_scons_tooling_metrics,
        utc_starttime,
        env_vars,
        env,
        parser,
        args,
        scons_exit_hook,
    )
