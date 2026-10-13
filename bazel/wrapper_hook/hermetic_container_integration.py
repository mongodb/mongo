"""Script entry point for the hermetic container integration used by tools/bazel."""

import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from bazel.wrapper_hook.hermetic_container import runner

if __name__ == "__main__":
    sys.exit(runner.main())
