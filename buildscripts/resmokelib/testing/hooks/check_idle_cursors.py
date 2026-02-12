import os.path

from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.utils import jscomment


class CheckIdleCursors(jsfile.JSHook):
    IS_BACKGROUND = False

    # Tag you’ll set in the JS test files that are allowed to leak cursors.
    ALLOW_LEAK_TAG = "can_leak_idle_cursors"

    def __init__(self, hook_logger, fixture, shell_options={}):
        description = "Checking for idle cursors in $currentOp"
        js_filename = os.path.join("jstests", "hooks", "jstest_infra", "run_check_idle_cursors.js")
        super().__init__(
            hook_logger, fixture, js_filename, description, shell_options=shell_options
        )

    def after_test(self, test, test_report):
        global_vars = self._shell_options.setdefault("global_vars", {})
        test_data = global_vars.setdefault("TestData", {})
        test_data["shouldKillIdleCursors"] = self._does_test_allow_leaking_idle_cursors(test)

        super().after_test(test, test_report)

    def _does_test_allow_leaking_idle_cursors(self, test):
        tags = jscomment.get_tags(test.test_name)
        return self.ALLOW_LEAK_TAG in tags
