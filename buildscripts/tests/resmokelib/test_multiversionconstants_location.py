"""Unit tests to ensure buildscripts/resmokelib/multiversionconstants.py location for the one-click repro tool."""
import importlib
import unittest


class TestMultiversionconstantsLocation(unittest.TestCase):
    def test_multiversionconstants_location(self):
        multiversionconstants_module_name = "buildscripts.resmokelib.multiversionconstants"
        try:
            under_test_module = importlib.import_module(multiversionconstants_module_name)
        except ImportError:
            self.fail(f"Failed to import `{multiversionconstants_module_name}` module. One-click"
                      f" repro tool (https://github.com/10gen/db-contrib-tools) requires this"
                      f" module. If the module was changed, one-click repro tool should also"
                      f" be updated. Please reach out in #server-testing slack channel.")
        else:
            expected_consts = ["LAST_LTS_FCV", "LAST_CONTINUOUS_FCV"]
            for const in expected_consts:
                self.assertTrue(
                    hasattr(under_test_module, const),
                    f"`{const}` constant is not found in `{multiversionconstants_module_name}`"
                    f" module. One-click repro tool (https://github.com/10gen/db-contrib-tools)"
                    f" uses this constant. If the module was changed, one-click repro tool"
                    f" should also be updated. Please reach out in #server-testing slack channel.")
