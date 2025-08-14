import unittest

from buildscripts.resmokelib import config
from buildscripts.resmokelib.parser import set_run_options
from buildscripts.resmokelib.suitesconfig import get_named_suites, get_suite


class TestSuitesConfigurations(unittest.TestCase):
    def test_validity_of_resmoke_suites_configurations(self):
        set_run_options("--runAllFeatureFlagTests")
        for suite_name in get_named_suites():
            try:
                suite = get_suite(suite_name)
                suite.tests
            except IOError as err:
                # We ignore errors from missing files referenced in the test suite's "selector"
                # section. Certain test suites (e.g. unittests.yml) have a dedicated text file to
                # capture the list of tests they run; the text file may not be available if the
                # associated bazel target hasn't been built yet.
                if err.filename in config.EXTERNAL_SUITE_SELECTORS:
                    continue
            except KeyError as e:
                # For example, `test_kind` might be missing from their yaml config
                self.fail(f"Error validating `{suite_name}` suite: Field {str(e)} not found.")
            except Exception as ex:
                self.fail(f"Error validating `{suite_name}` suite: {str(ex)}")
