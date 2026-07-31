import glob
import json
import os
import unittest
from collections import defaultdict
from typing import Optional

from buildscripts.idl.gen_all_feature_flag_list import get_all_feature_flags_turned_on_by_default
from buildscripts.resmokelib.multiversionconstants import (
    REQUIRES_FCV_TAG,
    REQUIRES_FCV_TAG_LATEST,
    REQUIRES_FCV_TAGS_LESS_THAN_LATEST,
    is_explicit_multiversion_test,
)
from buildscripts.resmokelib.utils import jscomment

# Tests that already carried an always-excluded requires_fcv tag when this rule was added. They
# are not running in any suite. Do not add to this list.
# TODO(SERVER-128742): Remove these tags and make the tests pass in the multiversion suites.
EXPLICIT_MULTIVERSION_FCV_TAG_ALLOWLIST = {
    "jstests/multiVersion/genericBinVersion/server-catalog-and-routing/replica_set_to_csrs_promotion_startup_flag.js",
    "jstests/multiVersion/genericChangeStreams/change_stream_v2_fcv_upgrade_downgrade.js",
    "jstests/multiVersion/genericChangeStreams/change_streams_timeseries_fcv_upgrade_downgrade.js",
    "jstests/multiVersion/genericChangeStreams/set_feature_compatibility_version_triggers_change_streams_retargeting.js",
    "jstests/multiVersion/genericSetFCVUsage/fcv_core/set_fcv_automatic_dryRun.js",
    "jstests/multiVersion/genericSetFCVUsage/fcv_core/set_fcv_dry_run_mode.js",
    "jstests/multiVersion/genericSetFCVUsage/migration_pending_recovery_drained_on_auth_shards_fcv_transition.js",
    "jstests/multiVersion/genericSetFCVUsage/priority_port_downgrade_checks.js",
}


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


class ExplicitMultiversionRequiresFcvTagRule(JstestTagRule):
    def __init__(self):
        super().__init__(
            failure_message=(
                "The following tests never run anywhere. They only run in the explicit "
                "multiversion suites, which Evergreen always invokes with "
                f"`--excludeWithAnyTags={REQUIRES_FCV_TAG}`. Remove the tag; to require newer "
                "binaries, check the binary version inside the test instead"
            )
        )
        self.disallowed_tags = set(REQUIRES_FCV_TAG.split(","))

    def _tag_failed(self, file: str, tag: str) -> bool:
        return (
            tag in self.disallowed_tags
            and is_explicit_multiversion_test(file)
            and file not in EXPLICIT_MULTIVERSION_FCV_TAG_ALLOWLIST
        )


class TestJstestTags(unittest.TestCase):
    def test_jstest_tags(self):
        os.chdir(os.environ.get("BUILD_WORKSPACE_DIRECTORY", "."))
        globs = ["src/mongo/db/modules/enterprise/jstests/**/*.js", "jstests/**/*.js"]

        tag_rules = [
            FeatureFlagIncompatibleTagRule(),
            RequiresFcvTagRule(),
            ExplicitMultiversionRequiresFcvTagRule(),
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
