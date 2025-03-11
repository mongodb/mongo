# Branching

This document describes branching task regarding file updates in `10gen/mongo` repository that should be done on a new branch after a branch cut.

## Table of contents

- [1. Create working branch](#1-create-working-branch)
- [2. Update files](#2-update-files)
  - [Copybara configuration](#copybara-configuration)
  - [YAML configurations](#yaml-configurations)
    - [Nightly YAML](#1-nightly-yaml)
    - [Misc YAML](#2-misc-yaml)
    - [Test Dev YAML](#3-test-dev-yaml)
    - [Sys-perf YAML](#4-sys-perf-yaml)
  - [Other files](#other-files)
- [3. Test changes](#3-test-changes)
- [4. Merge changes](#4-merge-changes)

## 1. Create working branch

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

- [8.1 Branching commit](https://github.com/10gen/mongo/commit/359cbe95216ffaf1a6173884f6519b3d408f1fb5) for reference.

### Copybara configuration

`copy.bara.sky` and `copy.bara.staging.sky`

- Update "master" branch references with a new branch name

### Evergreen YAML configurations

#### 1. Nightly YAML

- `etc/evergreen_nightly.yml` will be used as YAML configuration for a new `mongodb-mongo-vX.Y` evergreen project

- Copy over commit-queue aliases and patch aliases from `etc/evergreen.yml`
- Update "include" section - comment out or uncomment file includes as instructions in the comments suggest.

  This will move some build variants from `etc/evergreen.yml` to continue running on a new branch project.
  More information about build variants after branching is [here](../evergreen-testing/yaml_configuration/buildvariants.md#build-variants-after-branching).

#### 2. Misc YAML

- `etc/evergreen_yml_components/variants/misc/misc.yml`

- `burn_in_tags` configuration:
  - Remove build variant names from ["burn_in_tag_include_build_variants" expansion](https://github.com/mongodb/mongo/blob/0a68308f0d39a928ed551f285ba72ca560c38576/etc/evergreen_yml_components/variants/misc/misc.yml#L21) that are not included in `etc/evergreen_nightly.yml`

#### 3. Test Dev YAML

1. Promote build variants from "suggested" to "required"

   - Build variant names:
     - in `etc/evergreen_yml_components/variants/amazon/test_dev.yml`:
       - `enterprise-amazon-linux2023-arm64`

- Actions:

  - Replace "\*" with "!" in their display names
  - Replace "suggested" variant tag with "required"

2.  Remove all-feature-flags configuration from build variants

    - Build variant names:

      - in `etc/evergreen_yml_components/variants/windows/test_dev.yml`:
        - `enterprise-windows-all-feature-flags-required`
        - `enterprise-windows-all-feature-flags-non-essential`
      - in `etc/evergreen_yml_components/variants/sanitizer/test_dev.yml`:
        - `rhel8-debug-aubsan-lite-all-feature-flags-required`

    - Actions:

      - Remove `all-feature-flags` from their names and display names
      - Remove `--runAllFeatureFlagTests` from `test_flags` expansion
      - Replace `!.incompatible_all_feature_flags` tag selectors with `!.requires_all_feature_flags`

#### 4. Sys-perf YAML

- `etc/system_perf.yml` will be used as YAML configuration for a new `sys-perf-X.Y` evergreen project

  - Ensure that [DSI](https://github.com/10gen/dsi/blob/master/evergreen/system_perf/README.md#branching) and [Genny](https://github.com/mongodb/genny/blob/master/evergreen/system_perf/README.md#branching-the-mongo-repository) have been updated with new branches
  - Remove `evergreen/system_perf/master/master_variants.yml` from "include" section
  - Update `evergreen/system_perf/master/variants.yml` to `evergreen/system_perf/X.Y/variants.yml` in the "include" section
  - Update `evergreen/system_perf/master/genny_tasks.yml` to `evergreen/system_perf/X.Y/genny_tasks.yml` in the "include" section
  - Update the [evergreen project variable](https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Project-and-Distro-Settings#variables) `compile_project` in the new sys-perf-X.X evergreen project to point to the new mongodb-mongo-X branch

### Other files

- `buildscripts/generate_version_expansions.py`

  - Update [suffixes](https://github.com/10gen/mongo/blob/41ebdd14567ee35bdda0942958a5dc193f97dd5f/buildscripts/generate_version_expansions.py#L64-L65) from "latest" to "vX.Y-latest", where `vX.Y` is a new branch name

## 3. Test changes

In case working branch was created from `master` branch, rebase it on a new `vX.Y` branch and fix file conflicts if any.

Schedule required patch on a new `mongodb-mongo-vX.Y` project:

```sh
evergreen patch -p mongodb-mongo-vX.Y -a required
```

If patch results reveal that some steps are missing or outdated in this file, make sure to update the branching documentation on a "master" branch accordingly.

## 4. Merge changes

Open a Github PR to merge to branch `vX.Y`.
