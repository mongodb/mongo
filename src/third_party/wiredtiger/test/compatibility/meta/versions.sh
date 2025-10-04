#!/usr/bin/env bash
##############################################################################################
# This is the version list extracted from compatibility_test_for_releases
# The reason to extract this list out is to reuse it for python framework test
##############################################################################################

# Currently this is a temporary setup for python testsuite
# This will be merged with the following variables in the future
export SUITE_RELEASE_BRANCHES="develop mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"

# This array is used to configure the release branches we'd like to use for testing the importing
# of files created in previous versions of WiredTiger. Go all the way back to mongodb-4.2 since
# that's the first release where we don't support live import.
export IMPORT_RELEASE_BRANCHES="develop mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"

# Branches in below 2 arrays should be put in newer-to-older order.
#
# An overlap (last element of the 1st array & first element of the 2nd array)
# is expected to avoid missing the edge testing coverage.
#
# The 2 arrays should be adjusted over time when newer branches are created,
# or older branches are EOL.
export NEWER_RELEASE_BRANCHES="develop mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0 mongodb-5.0 mongodb-4.4"
export OLDER_RELEASE_BRANCHES="mongodb-4.4 mongodb-4.2"

# This array is used to generate compatible configuration files between releases, because
# upgrade/downgrade test runs each build's format test program on the second build's
# configuration file.
export COMPATIBLE_UPGRADE_DOWNGRADE_RELEASE_BRANCHES="mongodb-4.4 mongodb-4.2"

# This array is used to configure the release branches we'd like to run patch version
# upgrade/downgrade test.
export PATCH_VERSION_UPGRADE_DOWNGRADE_RELEASE_BRANCHES="mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0 mongodb-5.0 mongodb-4.4"

# This array is used to configure the release branches we'd like to run test checkpoint
# upgrade/downgrade test.
export TEST_CHECKPOINT_RELEASE_BRANCHES="develop mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0 mongodb-5.0 mongodb-4.4"

# This array is used to configure the release branches we'd like to run upgrade to latest test.
export UPGRADE_TO_LATEST_UPGRADE_DOWNGRADE_RELEASE_BRANCHES="mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0 mongodb-5.0 mongodb-4.4"
