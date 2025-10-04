#! /bin/sh

# This script is used as a workspace status command
#    bazel test --workspace_status_command=bazel/resmoke/workspace_status.sh
# to populate key-value pairs in bazel-out/volatile-status.txt.
# This file and the key-values can be consumed by bazel rules, but bazel
# pretends this file never changes when deciding what to rebuild.

# Evergreen expansions used primarily for Resmoke telemetry
echo build_id ${build_id}
echo distro_id ${distro_id}
echo execution ${execution}
echo project ${project}
echo revision ${revision}
echo revision_order_id ${revision_order_id}
echo task_id ${task_id}
echo task_name ${task_name}
echo build_variant ${build_variant}
echo version_id ${version_id}
echo requester ${requester}

# The current sets of enabled, disabled, and unrleased IFR feature flags. It
# would be better to remove this as it risks breaking the contract of
# volatile-status.txt. Changes to feature flag state should invalidate actions
# that consume this. SERVER-103590
python buildscripts/idl/gen_all_feature_flag_list.py feature-flag-status

python bazel/resmoke/get_historic_runtimes.py --project ${project:-""} --build-variant ${build_variant:-""} --task ${task_name:-""}
