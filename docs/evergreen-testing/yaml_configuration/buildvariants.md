# Build Variants

This document describes build variants (a.k.a. variants, or builds, or buildvariants) that are used
in `mongodb-mongo-*` projects. To know more about build variants, please refer to the
[Build Variants](https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Project-Configuration-Files#build-variants)
section of the Evergreen wiki.

## YAML files structure

Build variant configuration files are in `etc/evergreen_yml_components/variants` directory. They are
merged into `etc/evergreen.yml` and `etc/evergreen_nightly.yml` with Evergreen's
[include](https://docs.devprod.prod.corp.mongodb.com/evergreen/Project-Configuration/Project-Configuration-Files#include)
feature.

Inside `etc/evergreen_yml_components/variants` directory there are more directories, which are in
most cases platform names (e.g. amazon, rhel etc.) or build variant group names (e.g. sanitizer
etc.).

Be aware that some of these files could be also used or re-used to be merged into
`etc/system_perf.yml` which is used for `sys-perf` project.

## Build Variants in `mongodb-mongo-master` and `mongodb-mongo-master-nightly`

`mongodb-mongo-master` evergreen project uses `etc/evergreen.yml` and contains all build variants
for development, including all feature-specific, patch build required, and suggested variants.

`mongodb-mongo-master-nightly` evergreen project uses `etc/evergreen_nightly.yml` and contains build
variants for public nightly builds.

## Required and Suggested Build Variants

"Required" build variants are defined as any build variant with a `!` at the front of its display
name in Evergreen. These build variants also have `required` tag.

[Required Patch Builds Policy](https://wiki.corp.mongodb.com/display/KERNEL/Required+Patch+Builds+Policy)

"Suggested" build variants are defined as any build variant with a `*` at the front of its display
name in Evergreen. These build variants also have `suggested` tag.

## Build Variants with forbid_tasks_tagged_with_experimental

Build variants with the `forbid_tasks_tagged_with_experimental` tag indicate that they do not allow
tasks tagged as `experimental` to run. This tag is used in conjunction with the
`forbid-tasks-with-tag-on-variants` evergreen lint rule to enforce this restriction.

## Build Variants after branching

In each of platform or build variant group directory there can be these files:

| YAML file                             | Runs on           | Master project and Evergreen YML                     | Release project and Evergreen YML          |
| ------------------------------------- | ----------------- | ---------------------------------------------------- | ------------------------------------------ |
| `test_dev.yml`                        | master + releases | mongodb-mongo-master `evergreen.yml`                 | mongodb-mongo-vX.Y `evergreen_nightly.yml` |
| `test_dev_master_branch_only.yml`     | master only       | mongodb-mongo-master `evergreen.yml`                 | not used                                   |
| `test_release.yml`                    | master + releases | mongodb-mongo-master-nightly `evergreen_nightly.yml` | mongodb-mongo-vX.Y `evergreen_nightly.yml` |
| `test_release_master_branch_only.yml` | master only       | mongodb-mongo-master-nightly `evergreen_nightly.yml` | commented out                              |
