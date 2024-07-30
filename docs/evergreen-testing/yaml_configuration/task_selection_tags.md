# Task selection tags

This document describes task selection tags that are used in `mongodb-mongo-master` and `mongodb-mongo-master-nightly` projects.
To know more about task tags, please refer to the [Task and Variant Tags](https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Project-Configuration-Files#task-and-variant-tags) section of the Evergreen wiki.

The majority of variants in `mongodb-mongo-master-nightly` project and the most significat variants in `mongodb-mongo-master` project are using required and optional groups of task selection tags.
In order to add tasks to those variants, please use them as described in the following sections.

## Required task selection tags

Every task in `mongodb-mongo-master` and `mongodb-mongo-master-nightly` project must be tagged with exactly one required selection tag.
This is enforced by linter. YAML linter configuration could be found [here](../../../etc/evergreen_lint.yml).

- `development_critical` - these tasks should be green prior to the merge and will block merging if failing, e.g. jsCore.
  We run these tasks on all variants and in the commit-queue.

- `development_critical_single_variant` - the same as `development_critical` but these tasks do not require to run on multiple variants, e.g. clang-tidy, formatters, linters etc.
  We run these tasks on the required variant and in the commit-queue.

- `release_critical` - these tasks should be green prior to the release.
  We run these tasks on all release and development (required and suggested) variants.
  It should be uncommon to add tasks to this tag but if your task needs to run on many different OSes and it is extremely broad in coverage then you can add it to this tag.

- `default` - these tasks are running as part of a required patch build.
  We run these tasks on the most significant development variants (required patches, tsan, aubsan, etc.).
  Use this tag if you are not sure which tag to use for your new task.

- `non_deterministic` - these tasks depend significantly on randomization and we expect to see some unique failures, e.g. fuzzers etc.
  We run these tasks on non-required development variants.

- `experimental` - these tasks are not running anywhere regularly.
  We do not use this tag for selecting tasks to run on variants.
  This tag could be used for tasks that you would like to run on your own custom variants.

- `auxiliary` - these are various setup, helper, etc. tasks and should be mostly owned by infrastructure team.
  You should almost never use this tag.
  Please reach out to [#ask-devprod-build](https://mongodb.enterprise.slack.com/archives/CR8SNBY0N) before adding tasks with this tag.

**Important**: Do not change anything in this list without talking to [#ask-devprod-build](https://mongodb.enterprise.slack.com/archives/CR8SNBY0N).

## Optional task selection tags

In addition to the required task selection tags there is a list of optional selection tags.
Every task could be tagged with any number of the following tags:

- `incompatible_community` - the task should be excluded from the community variants.
- `incompatible_windows` - the task should be excluded from Windows variants.
- `incompatible_mac` - the task should be excluded from MacOS variants.
- `incompatible_ppc` - the task should be excluded from IBM PPC variants.
- `incompatible_s390x` - the task should be excluded from IBM s390x variants.
- `incompatible_debian` - the task should be excluded from debian variants.
- `incompatible_inmemory` - the task should be excluded from in-memory variants.
- `incompatible_aubsan` - the task should be excluded from {A,UB}SAN variants.
- `incompatible_tsan` - the task should be excluded from TSAN variants.
- `incompatible_debug_mode` - the task should be excluded from Debug Mode variants.
- `incompatible_system_allocator` - the task should be excluded from variants that use the system allocator.
- `incompatible_all_feature_flags` - the task should be excluded from all-feature-flags variants.
- `incompatible_development_variant` - the task should be excluded from the development variants.
- `incompatible_oscrypto` - the task should be excluded from variants unsupported by oscrypto.
- `requires_compile_variant` - the task can (or should) only run on variants that has compile releated expansions.
- `requires_large_host` - the task requires a large host to run.
- `requires_large_host_tsan` - the task requires a large host to run on TSAN variants.
- `requires_large_host_debug_mode` - the task requires a large host to run on Debug Mode variants.
- `requires_large_host_commit_queue` - the task requires a large host to run on in the commit-queue.
- `requires_all_feature_flags` - the task can only run on variants that has all-feature-flags configuration.
- `requires_execution_on_windows_patch_build` - the task should be run on the required Windows build variant on each patch
  build. See [SERVER-79037](https://jira.mongodb.org/browse/SERVER-79037) for how this was calculated.
