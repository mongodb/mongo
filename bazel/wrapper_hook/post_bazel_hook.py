# Hook to be called around bazel invocation time. Does not run on Windows.
import os
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from bazel.wrapper_hook.install_modules import install_modules

BAZEL_USER_NAMESPACE = "user-prod"
BAZEL_CI_NAMESPACE = "ci-prod"


def main():
    install_modules(sys.argv[1], sys.argv[1:])

    from bazel.wrapper_hook.flag_sync import sync_flags

    if os.environ.get("NO_FLAG_SYNC") is None:
        if os.environ.get("CI") is None:
            sync_flags(BAZEL_USER_NAMESPACE)
        else:
            sync_flags(BAZEL_CI_NAMESPACE)


if __name__ == "__main__":
    main()
