import sys

# pylint: disable=invalid-name
# pylint: disable=redefined-outer-name


# DO NOT INITIALIZE DIRECTLY -- This is intended to be a singleton.
class _ExitHook(object):
    """Plumb all sys.exit through this object so that we can access the exit code in atexit."""

    def __init__(self):
        self.exit_code = 0
        self._orig_exit = sys.exit
        sys.exit = self.exit

    def __del__(self):
        sys.exit = self._orig_exit

    def exit(self, code=0):
        self.exit_code = code
        self._orig_exit(code)


SINGLETON_TOOLING_METRICS_EXIT_HOOK = None


# Always use this method when initializing _ExitHook -- This guarantees you are using the singleton
# initialize the exit hook as early as possible to ensure we capture the error.
def initialize_exit_hook() -> None:
    """Initialize the exit hook."""
    try:
        if not SINGLETON_TOOLING_METRICS_EXIT_HOOK:
            SINGLETON_TOOLING_METRICS_EXIT_HOOK = _ExitHook()
    except UnboundLocalError as _:
        SINGLETON_TOOLING_METRICS_EXIT_HOOK = _ExitHook()
    return SINGLETON_TOOLING_METRICS_EXIT_HOOK
