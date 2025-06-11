import glob
import json
import os
import unittest
from collections import defaultdict
from typing import Optional

from buildscripts.idl.gen_all_feature_flag_list import get_all_feature_flags_turned_on_by_default
from buildscripts.resmokelib.multiversionconstants import (
    REQUIRES_FCV_TAG_LATEST,
    REQUIRES_FCV_TAGS_LESS_THAN_LATEST,
)
from buildscripts.resmokelib.utils import jscomment


class JstestTagRule:
    def __init__(self, failure_message):
        self.failure_message = failure_message
        self.failures = defaultdict(list)

    def check(self, file: str, tag: str) -> None:
        if self._tag_failed(file, tag):
            self.failures[file].append(tag)

    def _tag_failed(self, file: str, tag: str) -> bool:
        raise NotImplementedError()

    def make_failure_message(self) -> Optional[str]:
        if self.failures:
            pretty_failures = json.dumps(self.failures, indent=4)
            return f"{self.failure_message}:\n{pretty_failures}"
        return None


class FeatureFlagIncompatibleTagRule(JstestTagRule):
    def __init__(self):
        super().__init__(
            failure_message="The following tags are not allowed for feature flags that default to true"
        )
        self.disallowed_tags = {
            f"{flag}_incompatible" for flag in get_all_feature_flags_turned_on_by_default()
        }

    def _tag_failed(self, file: str, tag: str) -> bool:
        return tag in self.disallowed_tags


class RequiresFcvTagRule(JstestTagRule):
    def __init__(self):
        super().__init__(
            failure_message="The following tags reference FCV version that is not available"
        )
        self.allowed_tags = [*REQUIRES_FCV_TAGS_LESS_THAN_LATEST, REQUIRES_FCV_TAG_LATEST]

    def _tag_failed(self, file: str, tag: str) -> bool:
        return tag.startswith("requires_fcv_") and tag not in self.allowed_tags


class TestJstestTags(unittest.TestCase):
    def test_jstest_tags(self):
        os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))
        globs = ["src/mongo/db/modules/enterprise/jstests/**/*.js", "jstests/**/*.js"]

        tag_rules = [
            FeatureFlagIncompatibleTagRule(),
            RequiresFcvTagRule(),
        ]

        for pattern in globs:
            for file in glob.glob(pattern, recursive=True):
                for tag in jscomment.get_tags(file):
                    for tag_rule in tag_rules:
                        tag_rule.check(file, tag)

        full_failure_message = ""
        for tag_rule in tag_rules:
            failure_message = tag_rule.make_failure_message()
            if failure_message:
                full_failure_message = f"{full_failure_message}\n{failure_message}"

        if full_failure_message.strip():
            self.fail(full_failure_message.strip())
