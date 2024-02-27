# Evergreen configuration

This document describes the continuous integration (CI) configuration for MongoDB.

## Projects

There are a number of Evergreen projects supporting MongoDB's CI. For more information on
Evergreen-specific terminology used in this document, please refer to the
[Project Configuration](https://github.com/evergreen-ci/evergreen/wiki/Project-Configuration-Files)
section of the Evergreen wiki.

### `mongodb-mongo-master`

The main project for testing MongoDB's dev environments with a number build variants,
each one corresponding to a particular compile or testing environment to support development.
Each build variant runs a set of tasks; each task ususally runs one or more tests.

### `mongodb-mongo-master-nightly

Tracks the same branch as `mongodb-mongo-master`, each build variant corresponds to a
(version, OS, architecure) triplet for a supported MongoDB nightly release.

### `sys_perf`

The system performance project.

### `microbenchmarks`

Performance unittests, used mainly for validating areas related to the Query system.

## Project configurations

The above Evergreen projects are defined in the following files:

-   `etc/evergreen_yml_components/**.yml`. YAML files containing definitions for tasks, functions, buildvariants, etc.
    They are copied from the existing evergreen.yml file.

-   `etc/evergreen.yml`. Imports components from above and serves as the project config for mongodb-mongo-master,
    containing all build variants for development, including all feature-specific, patch build required, and suggested
    variants.

-   `etc/evergreen_nightly.yml`. The project configuration for mongodb-mongo-master-nightly, containing only build
    variants for public nightly builds, imports similar components as evergreen.yml to ensure consistency.

-   `etc/sys_perf.yml`. Configuration file for the system performance project.

-   `etc/perf.yml`. Configuration for the microbenchmark project.

## Release Branching Process

Only the `mongodb-mongo-master-nightly` project will be branched with required and other
necessary variants (e.g. sanitizers) added back in. Most variants in `mongodb-mongo-master`
would be dropped by default but can be re-introduced to the release branches manually on an
as-needed basis. For Rapid releases, all but the variants relevant to Atlas in
`mongodb-mongo-master-nightly` may be dropped as well.
