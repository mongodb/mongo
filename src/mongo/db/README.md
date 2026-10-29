# MongoDB Internals

This document aims to provide a high-level specification for the MongoDB's infrastructure to support
client/server interaction and process globals. Examples for such components are `ServiceContext` and
`OperationContext`. This is a work in progress and more sections will be added gradually.

## Server-Internal Baton Pattern

For details on the server-internal _Baton_ pattern, see [this document][baton].

[baton]: ../../../docs/baton.md

## FCV and Feature Flags

Feature Compatibility Version (FCV) and feature flags are the mechanisms used to safely roll out new
server behavior across upgrades, downgrades, and mixed-version clusters. The documentation is split
across the following pages:

- [README_fcv_overview.md](README_fcv_overview.md) — What FCV is, how it's set on startup, and how
  to check it in-memory or from a JS test.
- [README_setfcv_command.md](README_setfcv_command.md) — How the `setFeatureCompatibilityVersion`
  command works, including on sharded clusters, its dry-run mode, error cases, locks, and how to add
  upgrade/downgrade code to it.
- [README_fcv_generic_references.md](README_fcv_generic_references.md) — When it's acceptable to
  make a generic (non-release-specific) FCV reference, and the linter rule that enforces it.
- [README_fcv_constants_generation.md](README_fcv_constants_generation.md) — How FCV constants are
  generated at compile time from `releases.yml`, and what happens at branch cut.
- [README_feature_flags.md](README_feature_flags.md) — What feature flags are, the three flag styles
  (binary-compatible, IFR, FCV-gated), their lifecycle, how to create one, and the gating APIs.
- [README_multiversion_testing.md](README_multiversion_testing.md) — Overview of the multiversion
  and upgrade/downgrade test suites and passthroughs.
- [README_fcv_feature_flag_rules.md](README_fcv_feature_flag_rules.md) — A condensed summary of the
  rules for FCV and feature flag usage.

## VersionContext

For details on `VersionContext`, used to manage Operation FCV in sharding coordinators, see
[README_version_context.md](README_version_context.md).

## Default Command Time-Out

For details on `defaultMaxTimeMS` and `bypassDefaultMaxTimeMS`, see
[README_default_max_time_ms.md](README_default_max_time_ms.md).
