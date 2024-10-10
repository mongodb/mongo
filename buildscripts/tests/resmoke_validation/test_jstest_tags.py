import glob
import json
import unittest
from collections import defaultdict

from buildscripts.idl.gen_all_feature_flag_list import get_all_feature_flags_turned_on_by_default
from buildscripts.resmokelib.utils import jscomment


class TestJstestTags(unittest.TestCase):
    def test_jstest_tags(self):
        default_on_feature_flags = get_all_feature_flags_turned_on_by_default()

        disallowed_tags = {f"{flag}_incompatible" for flag in default_on_feature_flags}

        globs = ["src/mongo/db/modules/enterprise/jstests/**/*.js", "jstests/**/*.js"]
        failures = defaultdict(list)
        for pattern in globs:
            for file in glob.glob(pattern, recursive=True):
                for tag in jscomment.get_tags(file):
                    if tag in disallowed_tags:
                        failures[file].append(tag)

        if failures:
            pretty_failures = json.dumps(failures, indent=4)
            self.fail(
                f"The following tags are not allowed for feature flags that default to true:\n {pretty_failures}"
            )
