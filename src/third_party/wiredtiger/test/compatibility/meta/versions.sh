#!/usr/bin/env bash
##############################################################################################
# Version list used by the compatibility test suite.
#
# This file is the single source of truth for which release branches are included in each
# compatibility test type. It is sourced by compatibility_test_for_releases.sh and read by
# the Python test framework (common/compatibility_config.py).
#
# HOW TO ADD A NEW RELEASE BRANCH (minor or major)
# ------------------------------------------------
# When a new release branch is created (e.g. mongodb-8.3 or mongodb-9.0), add it to each of
# the arrays below. All arrays are in newer-to-older order; insert the new branch immediately
# after "develop" (or after the most recent minor of the same major series for minor releases).
#
#   SUITE_RELEASE_BRANCHES                            -- Python suite tests
#   IMPORT_RELEASE_BRANCHES                           -- import compatibility (-i)
#   NEWER_RELEASE_BRANCHES                            -- forward/backward/upgrade/downgrade (-n)
#   PATCH_VERSION_UPGRADE_DOWNGRADE_RELEASE_BRANCHES  -- patch-version test (-p)
#   TEST_CHECKPOINT_RELEASE_BRANCHES                  -- checkpoint recovery (part of -n)
#   UPGRADE_TO_LATEST_UPGRADE_DOWNGRADE_RELEASE_BRANCHES  -- upgrade-to-latest (-u) and dirty-restart (-d)
#
#
# HOW TO REMOVE AN EOL RELEASE BRANCH
# -----------------------------------
# Remove the branch from every array it appears in.
#
# After changing NEWER_RELEASE_BRANCHES, run the self-test to verify pair coverage is still correct:
#   ./compatibility_test_for_releases.sh -T
##############################################################################################

# Currently this is a temporary setup for python testsuite
# This will be merged with the following variables in the future
export SUITE_RELEASE_BRANCHES="develop mongodb-8.3 mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"

# This array is used to configure the release branches we'd like to use for testing the importing
# of files created in previous versions of WiredTiger.
export IMPORT_RELEASE_BRANCHES="develop mongodb-8.3 mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"

export NEWER_RELEASE_BRANCHES="develop mongodb-8.3 mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"

# This array is used to configure the release branches we'd like to run patch version
# upgrade/downgrade test.
export PATCH_VERSION_UPGRADE_DOWNGRADE_RELEASE_BRANCHES="mongodb-8.3 mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"

# This array is used to configure the release branches we'd like to run test checkpoint
# upgrade/downgrade test.
export TEST_CHECKPOINT_RELEASE_BRANCHES="develop mongodb-8.3 mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"

# This array is used to configure the release branches we'd like to run upgrade to latest test.
export UPGRADE_TO_LATEST_UPGRADE_DOWNGRADE_RELEASE_BRANCHES="mongodb-8.3 mongodb-8.2 mongodb-8.0 mongodb-7.0 mongodb-6.0"
