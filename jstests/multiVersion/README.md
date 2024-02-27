# Multiversion Tests

## Context

These tests test specific upgrade/downgrade behavior expected between
different versions of MongoDB. Some of these tests will persist indefinitely & some of these tests
will be removed upon branching. All targeted tests must go in a targeted test directory. Do not put
any files in the multiVersion/ top-level directory.

## Generic Tests

These tests test the general functionality of upgrades/downgrades regardless
of version. These will persist indefinitely, as they should always pass regardless
of MongoDB version.

## Targeted Tests

These tests are specific to the current development cycle. These can/will fail after branching and
are subject to removal during branching.

### targetedTestsLastLtsFeatures

These tests rely on a specific last-lts version. After the next major release, last-lts is a
different version than expected, so these are subject to failure. Tests in this directory will be
removed after the next major release.

### targetedTestsLastContinuousFeatures

These tests rely on a specific last-continuous version. After the next minor release,
last-continuous is a different version than expected, so these are subject to failure. Tests in
this directory will be removed after the next minor release.
