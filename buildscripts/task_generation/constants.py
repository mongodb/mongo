"""Task-generation relaed constants."""
import os
import re

CONFIG_FILE = ".evergreen.yml"
EVERGREEN_FILE = "etc/evergreen.yml"

ARCHIVE_DIST_TEST_TASK = "archive_dist_test"
ARCHIVE_DIST_TEST_DEBUG_TASK = "archive_dist_test_debug"
ACTIVATE_ARCHIVE_DIST_TEST_DEBUG_TASK = "activate_archive_dist_test_debug"
DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")
MAX_WORKERS = 16
LOOKBACK_DURATION_DAYS = 14
MAX_TASK_PRIORITY = 99
GENERATED_CONFIG_DIR = "generated_resmoke_config"
EXCLUDES_TAGS_FILE = "multiversion_exclude_tags.yml"
EXCLUDES_TAGS_FILE_PATH = os.path.join(GENERATED_CONFIG_DIR, EXCLUDES_TAGS_FILE)
GEN_PARENT_TASK = "generator_tasks"
EXPANSION_RE = re.compile(r"\${(?P<id>[a-zA-Z0-9_]+)(\|(?P<default>.*))?}")
BACKPORT_REQUIRED_TAG = "backport_required_multiversion"

# evergreen.yml function names.
CONFIGURE_EVG_CREDENTIALS = "configure evergreen api credentials"
DO_MULTIVERSION_SETUP = "do multiversion setup"
RUN_GENERATED_TESTS = "run generated tests"
RUN_TESTS = "run tests"
