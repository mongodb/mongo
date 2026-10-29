# Summary of Rules for FCV and Feature Flag Usage

> Part of the [FCV and Feature Flag documentation](README.md#fcv-and-feature-flags).

- Any project or ticket that wants to introduce different behavior based on which FCV the server is
  running **_must_** add an FCV-gated feature flag.
- We should **_not_** be adding any references to FCV constants such as kVersion_6_0.
- To make sure all generic FCV references are indeed meant to exist across LTS binary versions, **_a
  comment containing “(Generic FCV reference):” is required within 10 lines before a generic FCV
  reference._**
- Operations that check the FCV or FCV-gated feature flags must take the global IX or X lock before
  doing so. This ensures that operation won't outlast a full upgrade/downgrade, and that the
  operation serializes correctly with the metadata cleanup steps of setFCV.
- Except when using a binary-compatible feature flag, an operation should not check a feature flag
  more than once, because the flag's state may change between checks.
- Any projects/tickets that use and enable an FCV-gated feature flag **_must_** leave that feature
  flag in the codebase at least until the next major release.
- There is no support for disabling a feature once it is released by setting its `default` property
  to `true` (for binary-compatible and FCV-gated feature flags) or setting its
  `incremental_rollout_phase` to `release` (for IFR feature flags).
- Feature flags should never be enabled in the period of time after branch cut but before FCV
  constants have been upgraded to the next version.
- In general, tag tests that depend on a feature flag with `featureFlagXX` and `requires_fcv_yy`
  tags, where `yy` is the FCV that the feature flag is/will be enabled by default on. Tests in
  explicit multiversion suites (e.g. tests in `jstests/multiVersion`) are the exception: they must
  not carry a `requires_fcv_yy` tag, which would exclude them from every run.
