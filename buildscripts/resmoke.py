#!/usr/bin/env python3
"""Command line utility for executing MongoDB tests of all kinds."""

import os.path
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    mongo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # use the mongo root directory to resolve depencies first
    # This is so when resmoke is used as a module in an external project,
    # we use the resmoke code that is bundled with resmoke,
    # not a local checkout of resmoke that might be for a different version
    sys.path.insert(0, mongo_root)
    if os.path.normpath(mongo_root) != os.path.normpath(os.getcwd()):
        # If the current working directory is not the mongo root that means
        # we are running as an external module.
        # We need to add that path so we can import fixtures from the external location.
        # This needs to be a lower priority than the mongo root.
        sys.path.insert(1, os.getcwd())


try:
    from buildscripts.resmokelib import cli
except Exception as ex:
    if not os.getenv("VIRTUAL_ENV") and not os.getenv("BUILD_WORKSPACE_DIRECTORY"):
        venv_not_activated = "You need to activate your virtual environment"
        # Python versions >= 3.11 support an add_note() function on exceptions so this
        # message shows up _after_ the stack trace (more likely to be seen by a dev)
        if sys.version_info.major == 3 and sys.version_info.minor >= 11:
            ex.add_note(venv_not_activated)
        else:
            print(venv_not_activated)
    raise


# Entrypoint
def entrypoint():
    cli.main(sys.argv)


if __name__ == "__main__":
    entrypoint()
