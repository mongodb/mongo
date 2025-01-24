import os
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

# This script should be careful not to disrupt automatic mechanism which
# may be expecting certain stdout, always print to stderr.
sys.stdout = sys.stderr

from bazel.wrapper_hook.engflow_check import engflow_auth
from bazel.wrapper_hook.install_modules import install_modules
from bazel.wrapper_hook.plus_interface import test_runner_interface
from bazel.wrapper_hook.wrapper_debug import wrapper_debug

wrapper_debug(f"wrapper hook script is using {sys.executable}")


def main():
    install_modules(sys.argv[1])

    engflow_auth(sys.argv)

    args = test_runner_interface(
        sys.argv[1:], autocomplete_query=os.environ.get("MONGO_AUTOCOMPLETE_QUERY") == "1"
    )
    os.chmod(os.environ.get("MONGO_BAZEL_WRAPPER_ARGS"), 0o644)
    with open(os.environ.get("MONGO_BAZEL_WRAPPER_ARGS"), "w") as f:
        f.write(" ".join(args))


if __name__ == "__main__":
    main()
