#! /bin/sh

# This script is used as a workspace status command
#    bazel test --workspace_status_command=bazel/resmoke/workspace_status.sh
# to populate key-value pairs in bazel-out/volatile-status.txt and stable-status.txt.
# These files and the key-values can be consumed by bazel rules, but bazel
# pretends volatile-status never changes when deciding what to rebuild.

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
echo otel_trace_id ${otel_trace_id}
echo otel_parent_id ${otel_parent_id}

python bazel/resmoke/workspace_status.py
