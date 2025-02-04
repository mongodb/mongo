import os
import sys

if (
    os.environ.get("MONGO_BAZEL_WRAPPER_DEBUG") == "1"
    and os.environ.get("MONGO_AUTOCOMPLETE_QUERY") != "1"
):

    def wrapper_debug(x):
        print("[WRAPPER_HOOK_DEBUG]: " + x, file=sys.stderr)
else:

    def wrapper_debug(x):
        pass
