# Branching

This document describes branching task regarding file updates in `10gen/mongo` repository that should be done on a new branch immediately after a branch cut.

## Table of contents

1. [Project settings](#1-project-settings)
1. [Create working branch](#2-create-working-branch)
1. [Update files](#3-update-files)
1. [Test changes](#4-test-changes)
1. [Merge changes](#5-merge-changes)

## 1. Project settings

### GitHub App credentials

Add GitHub app credentials (app id and key) in the new project settings, eg. https://spruce-beta.corp.mongodb.com/project/mongodb-mongo-v8.3/settings/github-app-settings (additional MANA permissions may be required, else coordinate with Release team contacts).

## 2. Create working branch

To save time during the branch cut these branching changes could be done beforehand, but not too early to avoid extra file conflicts, and then rebased on a new `vX.Y` branch.

Create a working branch from `master` or from a new `vX.Y` branch if it already exists:

```sh
git checkout master
git pull
git checkout -b vX.Y-branching-task
```

## 2. Update files

**IMPORTANT!** All of these changes should be a separate commit, but they should be pushed together in the same commit-queue task.

The reason they should be pushed as separate commits is in the case of needing to revert one aspect of this entire task.

> See [8.2 branching PR](https://github.com/mongodb/mongo/pull/38920/commits) for reference.

Some have some automated steps you can run, but please double-check their edits. Initialize the version here, used throughout:

```sh
VERSION=8.3
```

### Copybara configuration

Run the following automation and verify results:

```sh
sed -i "s/master/v$VERSION/g" copy.bara.sky buildscripts/sync_repo_with_copybara.py
```

For each file [`copy.bara.sky`](../../copy.bara.sky) and [`sync_repo_with_copybara.py`](../../buildscripts/sync_repo_with_copybara.py), the "master" branch references should be replaced with the new branch name.

### Evergreen YAML configurations

### 1. Version expansions

Run the following automation and verify results:

```sh
sed -i "s/suffix\"] = \"latest\"/suffix\"] = \"v$VERSION-latest\"/g" buildscripts/generate_version_expansions.py
```

In the file [`buildscripts/generate_version_expansions.py`](../../buildscripts/generate_version_expansions.py), the "latest" suffixes should be replaced with the new branch name.

#### 2. Nightly YAML

[`etc/evergreen_nightly.yml`](../../etc/evergreen_nightly.yml) will be used as YAML configuration in the new `mongodb-mongo-vX.Y` evergreen project.

This will move some build variants from `etc/evergreen.yml` to continue running on a new branch project. More information about build variants after branching is [here](../evergreen-testing/yaml_configuration/buildvariants.md#build-variants-after-branching).

- Copy over commit-queue aliases and patch aliases from [`etc/evergreen.yml`](../../etc/evergreen.yml)
- Update "include" section: comment out or uncomment file includes as instructions in the comments suggest.

#### 3. Burn-in tasks

Run the following automation and verify results:

```sh
sed -i '/burn_in_tag_include_build_variants/{N;N;N;d;}' etc/evergreen_yml_components/variants/misc/misc.yml
```

In the file [`etc/evergreen_yml_components/variants/misc/misc.yml`](../../etc/evergreen_yml_components/variants/misc/misc.yml), build variant names in the ["burn_in_tag_include_build_variants" expansion](https://github.com/mongodb/mongo/blob/0a68308f0d39a928ed551f285ba72ca560c38576/etc/evergreen_yml_components/variants/misc/misc.yml#L21) that are _not_ included in [`etc/evergreen_nightly.yml`](../../etc/evergreen_nightly.yml) are _removed_.

#### 4. Suggested to Required

Run the following automation and verify results:

```sh
sed -i 's@display_name: "\* Amazon Linux 2023 arm64 Enterprise"@display_name: "! Amazon Linux 2023 arm64 Enterprise"@g' etc/evergreen_yml_components/variants/amazon/test_dev.yml

sed -i 's/tags: \["suggested", "forbid_tasks_tagged_with_experimental"\]/tags: ["required", "forbid_tasks_tagged_with_experimental"]/g' etc/evergreen_yml_components/variants/amazon/test_dev.yml
```

For the variant `enterprise-amazon-linux2023-arm64` in [`etc/evergreen_yml_components/variants/amazon/test_dev.yml`](../../etc/evergreen_yml_components/variants/amazon/test_dev.yml), replace:

- "\*" with "!" in their display names
- "suggested" variant tag with "required"

#### 5. Feature flags

Remove all-feature-flags configuration from build variants

Run the following automation and verify results:

```sh
FILES='etc/evergreen_yml_components/variants/sanitizer/test_dev.yml etc/evergreen_yml_components/variants/windows/test_dev.yml'

sed -i 's/ (all feature flags)//g' $FILES
sed -i 's/-all-feature-flags//g' $FILES
sed -i '/--runAllFeatureFlagTests/d' $FILES
sed -i 's/!.incompatible_all_feature_flags/!.requires_all_feature_flags/g' $FILES
```

For the build variant names:

- in [`etc/evergreen_yml_components/variants/windows/test_dev.yml`](../../etc/evergreen_yml_components/variants/windows/test_dev.yml):
  - `enterprise-windows-all-feature-flags-required`
  - `enterprise-windows-all-feature-flags-non-essential`
- in [`etc/evergreen_yml_components/variants/sanitizer/test_dev.yml`](../../etc/evergreen_yml_components/variants/sanitizer/test_dev.yml):

  - `linux-debug-aubsan-lite-all-feature-flags-required`

- It should:
  - Remove `all-feature-flags` from their names and display names
  - Remove `--runAllFeatureFlagTests` from `test_flags` expansion
  - Replace `!.incompatible_all_feature_flags` tag selectors with `!.requires_all_feature_flags`

#### 6. Sys-perf YAML

[`etc/system_perf.yml`](../../etc/system_perf.yml) will be used as YAML configuration for a new `sys-perf-X.Y` evergreen project

> Ensure that [DSI](https://github.com/10gen/dsi/blob/master/evergreen/system_perf/README.md#branching) has been updated with new branches

Run the following automation and verify results:

```sh
sed -i '/evergreen\/system_perf\/master\/master_variants.yml/{N;d;}' etc/system_perf.yml

sed -i "s@evergreen/system_perf/master/compiles.yml@evergreen/system_perf/$VERSION/compiles.yml@g" etc/system_perf.yml
sed -i "s@evergreen/system_perf/master/variants.yml@evergreen/system_perf/$VERSION/variants.yml@g" etc/system_perf.yml
```

In the file [`etc/system_perf.yml`](../../etc/system_perf.yml), the following should be reflected:

- Remove `evergreen/system_perf/master/master_variants.yml` from "include" section
- With the exception of `base.yml`, update all other entries that contain `master` in the path to contain `X.Y` in the path instead. (e.g. `evergreen/system_perf/master/variants.yml` should become `evergreen/system_perf/X.Y/variants.yml`).
- Update the [evergreen project variable](https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Project-and-Distro-Settings#variables) `compile_project` in the new sys-perf-X.Y evergreen project to point to the new mongodb-mongo-vX.Y branch

#### 7. Evergreen project validation

Run the following automation and verify results:

```sh
sed -i 's/RELEASE_BRANCH = False/RELEASE_BRANCH = True/g' buildscripts/validate_evg_project_config.py
```

In file [`buildscripts/validate_evg_project_config.py`](../../buildscripts/validate_evg_project_config.py), the `RELEASE_BRANCH` variable should be set to `True` to leverage a specialized shortcut conditional to `evaluate` the project, not `validate`.

#### 8. Coverity

Run the following automation and verify results:

```sh
sed -i "s/stream: mongo.master/stream: mongo.v$VERSION/g" etc/coverity.yml
```

In the file [`etc/coverity.yml`](../../etc/coverity.yml), the "stream" should be updated to the new branch.

#### Finally: format and lint

```sh
bazel run lint format
```

Run linters and formatters and fix anything that couldn't be autofixed.

## 3. Test changes

In case working branch was created from `master` branch, rebase it on a new `vX.Y` branch and fix file conflicts if any.

Schedule required patch on a new `mongodb-mongo-vX.Y` project:

```sh
evergreen patch -p mongodb-mongo-vX.Y -a required
```

If patch results reveal that some steps are missing or outdated in this file, make sure to update the branching documentation on a "master" branch accordingly.

## 4. Merge changes

Open a Github PR to merge to branch `vX.Y`.
