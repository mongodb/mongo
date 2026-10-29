# Feature Flags

> Part of the [FCV and Feature Flag documentation](README.md#fcv-and-feature-flags). See also:
> [README_version_context.md](README_version_context.md) and
> [README_query_feature_flags.md](query/README_query_feature_flags.md).

## What are Feature Flags

Feature flags are a technique to support new server features that are still in active development on
the master branch. This technique allows us to iteratively commit code for a new server feature
without the risk of accidentally enabling the feature for a scheduled product release.

The criteria for determining whether a feature flag can be enabled by default and released is
largely specific to the scope and design but ideally a feature can be made available when:

- There is sufficient test coverage to provide confidence that the feature works as designed.
- Upgrade/downgrade issues have been addressed.
- The feature does not destabilize the server as a whole while running in our CI system.

## When to use Feature Flags

Feature flags are a requirement for continuous delivery, thus features that are in development must
have a feature flag. Features that span multiple commits should also have a feature flag associated
with them because continuous delivery means that we often branch in the middle of feature
development.

There are three types of feature flags, designed for three distinct use cases:

- _Binary-compatible_ feature flags are used to keep backwards-compatible features disabled until
  their development is completed and they can be confidently enabled.
- _Incremental feature rollout_ feature flags control features that are deployed using the
  Incremental Feature Rollout (IFR) process, which gradually introduces a feature to a fleet of
  clusters by turning the feature flag on in selected clusters and, if needed, can _roll back_
  deployment by toggling the flag back off.
- _FCV-gated_ feature flags are for those features that have compatibility or upgrade/downgrade
  concerns and must be kept disabled until FCV reaches a certain threshold version.

For more details and guidelines on determining which style of flag is appropriate, see
[Determining which style of feature flag to use](#determining-which-style-of-feature-flag-to-use).

It should be noted that any project or ticket that wants to introduce different behavior based on
which FCV the server is running **_must_** add an FCV-gated feature flag. In the past, the branching
of the different behavior would be done by directly checking which FCV the server was running.
However, we now must **_not_** be using any references to FCV constants such as kVersion*6_0
([example of what to avoid](https://github.com/mongodb/mongo/blob/ef8bdb8d0cbd584d47c54d64c3215ae29ec1a32f/src/mongo/db/pipeline/document_source_list_catalog.cpp#L130)).
Instead we should branch the different behavior using feature flags (see
[Feature Flag Gating](#feature-flag-gating)). \*\*\_This means that individual ticket that wants to
introduce an FCV check will also need to create a feature flag specific to that ticket.*\*\*

The motivation for using FCV-gated feature flags rather than checking FCV constants directly is
because checking FCV constants directly is more error prone and has caused issues in the release
process when updating/removing outdated FCV constants.

## Dynamically toggling features

Feature flags are not intended to control behaviors that users may want to enable or disable as part
of normal operation. They are enabled as part of the release process and, once enabled, are rarely,
if ever, disabled again. Feature flags primarily hide in-development features from production builds
(enabling continuous deployment) and secondarily coordinate feature deployment during cluster
upgrade.

In production, binary-compatible feature flags have a fixed value that is baked into the release
(defined by the `default` property in the flag's IDL specification), and there is no supported means
to change that value. Tests can choose a value for a binary-compatible flag at startup but cannot
change the value for a running process. Feature authors can safely assume that a binary-compatible
flag will have the same value for the lifetime of a process.

The state of an FCV-gated or IFR feature flag _can_ change during the lifetime of a process,
however. An enabled-by-default FCV-gated flag can switch between enabled and disabled at runtime as
part of FCV upgrade and downgrade, and an IFR flag can switch at any time as a result of a
`setParameter` issued by the incremental rollout procedure.

A feature implementation hidden by an FCV-gated or IFR flag must be able to tolerate concurrent
changes to the flag's state. Any operation that accesses the feature flag should be careful not to
assume that multiple checks will produce the same result.

In almost all cases, an individual operation (e.g., a query or DDL command) should access a feature
flag _only once_. If the operation needs a flag's state at multiple times during its execution, it
should check the flag once and save the result for future use, effectively snapshotting it, rather
than accessing the flag each time. The
[FCV-gated feature flag API](#fcv-gated-feature-flag-api-fcvgatedfeatureflag) and
[IFR feature flag API](#ifr-feature-flag-api-incrementalrolloutfeatureflag) both provide affordances
for this pattern in the form of the `VersionContext` and the `IncrementalFeatureRolloutContext`,
respectively.

## Determining which style of feature flag to use

Usually a feature will need to use an FCV-gated feature flag when the feature's behavior has a
meaningful effect outside of a single process. Specifically,

1. the feature changes the on-disk format of some data or
2. the feature changes the way nodes in a cluster communicate with each other.

### On-disk Format Risk

In the first case, FCV gating is needed to ensure that a binary-upgraded cluster can still be
restored to its previous binary version until the upgrade is finalized via the FCV upgrade process.
Before the FCV transition completes, the feature remains disabled so that it will not write data
that is incompatible with the previous binary version.

### Wire Protocol Risk

In the second case, FCV gating allows an enabled feature to assume that no nodes with an older
binary version remain in the cluster, so it can send commands and write oplog entries that older
versions would not be able to interpret.

Please note that this includes:

- additions and changes to the query language and
- changes to the serialization format of query operators.

Not only are these operators serialized across the wire between nodes, they also can be written to
oplog entries as part of non-materialized views or collection validators. If you are on the query
team, please read more at
[README_query_feature_flags.md](/src/mongo/db/query/README_query_feature_flags.md).

> 🚧 Caution
>
> Just because a feature only executes as part of shard routing does not mean it only executes on
> mongos. A mongod instance can act as the router for a query that originates from a data-bearing
> node.

### Incremental Feature Rollout Flags

Features that do not need an FCV gate may be suited to deployment via IFR feature flag. Examples
include

- features targeting query performance or plan selection that do not affect query semantics,
- features that add node-level diagnostics or validation,
- features that only execute on mongos, which does not have an FCV.

An FCV-independent feature can use an IFR feature flag so long as it is written to tolerate runtime
changes to the flag state that can occur at any time as
[described above](#dynamically-toggling-features).

> ❗️ Note
>
> Incremental Feature Rollout is still in development. Reach out to the IFR team (see SERVER-89518)
> before adding an IFR feature flag.

An FCV-independent feature that is not practical to implement to IFR requirements can instead use a
binary-compatible feature flag, avoiding the need to account for the possibility of the feature
becoming enabled or disabled at runtime.

If in doubt about whether your feature needs to be FCV gated, please reach out to the Catalog and
Routing team in #server-featureflags and add the Catalog and Routing team as a reviewer to your code
review.

## Lifecycle of a feature flag

- Adding the feature flag

  - Disabled by default. This minimizes disruption to the CI system and BB process.
  - This should be done concurrently with the first work ticket in the PM for the feature.

- Enabling the feature by default

  - Enable an FCV-gated or binary-compatible feature flag by updating its specification with
    `default: true`.
  - Enable an IFR feature flag by changing the `incremental_rollout_phase` property in its
    specification from `in_development` to `rollout`, which will automatically make it enabled by
    default.
    - After rollout completes, change this property to `release` on the master branch but not on any
      release branches.
  - FCV-gated feature flags with `default: true` must have a specific release version associated in
    its definition.
  - **_We do not support disabling feature flags once they have been enabled via IDL in a release
    build_**.
  - **_Feature flags must not be enabled after a branch cut if the FCV has not yet been upgraded_**.
    This is because before FCVs are upgraded on master, we will temporarily have two builds that
    have the same version (the newly cut branch and master). If the two builds differ on which
    feature flags are enabled, this will lead to test failures in certain multiversion suites.
  - Project team should run a full "required" alias patch. Double check that all tasks for all build
    variants prefixed with "!" are selected.
  - If there are downstream teams that will break when this flag is enabled then enabling the
    feature by default will need to wait until those teams have affirmed they have adapted their
    product.
  - JS tests tagged with this feature flag should remove the `featureFlagXX` tag. The test should
    add a `requires_fcv_yy` tag to ensure that the test will not be run in incompatible multiversion
    configurations. (The `requires_fcv_yy` tag is enforced as of
    [SERVER-55858](https://jira.mongodb.org/browse/SERVER-55858)). See the
    [Feature Flag Test Tagging](#feature-flag-test-tagging) section for more details.
    - Exception: for a test in an explicit multiversion suite (e.g. a test in
      `jstests/multiVersion`), only remove the `featureFlagXX` tag. Do not add `requires_fcv_yy` -
      those tests run in no other suite, so the tag stops the test from running anywhere. If the
      test needs newer binaries, check the binary version inside the test instead.
  - After this point the feature flag is used for FCV gating to make upgrade/downgrade safe.

- Removing the feature flag
  - Any projects/tickets that use and enable an FCV-gated feature flag **_must_** leave that feature
    flag in the codebase at least until the next major release. _For example, if a feature flag was
    enabled by default in 5.1, it must remain in the codebase until 6.0 is branched. After that, the
    feature flag can be removed._ This is because the feature flag is used for FCV gating during the
    upgrade/downgrade process. For example, if a feature is completed and the feature flag is
    enabled by default in FCV 5.1, then from binary versions 5.1, 5.2, 5.3, and 6.0, the server
    could have its FCV set to 5.0 during the downgrade process, where the feature is not supposed to
    be enabled. If the feature flag was removed earlier than 6.0 then the feature would still run
    upon downgrading the FCV to 5.0.

## Creating a Feature Flag

Create a feature flag by adding its specification to an IDL file:

```yaml
global:
  cpp_namespace: "mongo"

feature_flags:
  # featureFlagFork is a binary-compatible feature flag that is finished and enabled by default.
  featureFlagFork:
    description: "Fork feature flag"
    cpp_varname: feature_flags::gFeatureFlagFork
    default: true
    fcv_gated: false

  # featureFlagToaster is an FCV-gated feature flag that is under development and off by default.
  # This feature flag can be enabled only after associating it with the latest FCV (eg. 4.9.0).
  featureFlagToaster:
    description: "Toaster feature flag"
    cpp_varname: feature_flags::gFeatureFlagToaster
    default: false
    fcv_gated: true
```

A feature flag has the following properties:

- Server Parameter Name: featureFlag\<MyFeatureName\>
  - Feature flags implicitly generate a server parameter (see `FeatureFlagServerParameter`).
  - The name should not include "Use", "Allow", "Enable", or "Disable".
  - Feature flags can only be set at server startup: their server parameters are declared with
    `set_at: startup`. This supports the use of feature flags as a release/development tool and
    should not impose an unnecessary burden on server administration.
- C++ Namespace: `mongo::feature_flags`
  - This should apply to all feature flags that follow the naming convention in the previous bullet.
  - By convention all feature flags should be placed in this namespace. For server parameters that
    support multiple enumerated choices (for example, protocol/implementation for a command), these
    should be declared separately. Feature flags may still be used to toggle support for a
    particular option for the non-feature flag server parameter.
- C++ Name: gFeatureFlagToaster
- Default Value: false
  - The default value for a new feature flag should always start with false.
  - When the feature is fully tested and ready for release, we can change the default to true. At
    that time, a `version` must be specified.
- Version: string - a string for the FCV version
  - Required field if `default` is true and `fcv_gated` is true
  - Must be a string acceptable to FeatureCompatibilityVersionParser.
- fcv_gated: boolean
  - Set to `true` to define an FCV-gated feature flag. Set to `false` for a binary-compatible or IFR
    feature flag.
  - When `fcv_gated` is `true` and the flag specification includes a `version` property, defining a
    minimum FCV requirement, the feature only becomes enabled once the cluster's FCV is upgraded to
    meet the requirement. In supported configurations, a mongod that observes an FCV-gated flag to
    be enabled can safely assume that the flag is enabled on every mongod in the cluster. (mongos
    processes do not know the cluster FCV.)
  - When `fcv_gated` is `false`, the feature must be able to tolerate operating in a heterogeneous
    cluster, in which some nodes have not yet enabled the feature and/or some nodes have not yet
    been upgraded to a binary version that includes the feature.
  - Must be `false` if `incremental_rollout_phase` has any value other than the default. A feature
    flag cannot be both an IFR flag and an FCV-gated flag.
  - See
    [Determining if a feature flag should be binary-compatible or FCV-gated](#determining-which-style-of-feature-flag-to-use)
    for guidelines on when each type of feature flag should be used.
- incremental_rollout_phase: (`not_for_incremental_rollout`|`in_development`|`rollout`|`release`)
  - Optional. Use the default value `not_for_incremental_rollout` for FCV-gated and
    binary-compatible feature flags.
  - To define an IFR flag, specify a value other than the default.
    - `in_development`: The flag will be disabled by default in testing and production.
    - `rollout`: The flag will be enabled by default at startup. In production, the incremental
      rollout procedure may temporarily disable the feature.
    - `release`: The flag will be enabled by default in testing and production.
  - Only the default value `not_for_incremental_rollout` is valid when `fcv_gated` is true. A
    feature flag cannot be both an IFR flag and an FCV-gated flag.
  - The `default` property is optional for IFR feature flags. It is an error to specify a `default`
    value that does not match the rollout phase (`false` for `in_development` and `true` for
    `rollout` or `release`).
- enable_on_transitional_fcv_UNSAFE: boolean
  - Optional. Can only be specified for FCV-gated feature flags (`fcv_gated: true`). Default value
    is `false`.
  - When set to `true`, an FCV-gated feature flag will also be enabled during
    `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_Y_To_X`, where `Y` is the version specified in
    `version`. For example, if a feature flag specifies `version: 8.0` and
    `enable_on_transitional_fcv_UNSAFE: true`, then it will be enabled during
    `kUpgradingFrom_7_0_To_8_0` or `kDowngradingFrom_8_0_To_7_0`. Notably, it won't be enabled
    during `kUpgradingFrom_7_0_To_7_3`.
  - Its intended use is deploying complex features in a staged fashion: A feature flag is enabled
    during the transition phase in order to prepare some aspects of a feature, along with another
    feature flag that is enabled in the fully upgraded FCV and enables the feature's functionality.
  - This option must not be used as a general way to deploy features because it drops many of the
    safeguards provided by setFCV for feature flags, introducing many pitfalls such as:
    - Upon starting a upgrade, an operation may find the feature flag enabled while operations on
      the fully downgraded FCV (`kVersion_X`) still run (before the global lock barrier & draining).
      Since that FCV is already released, so there is no wiggle room to change its behavior.
    - On a sharded cluster, the flag may be enabled on one shard, while others shards are still on
      the fully downgraded FCV (`kVersion_X`).
    - Upon finishing a downgrade, an operation may have checked a feature flag as enabled, then the
      FCV may have transitioned to fully downgraded (`kVersion_X`). Persisting data such as oplog
      entries in new formats after this can cause incompatibilities if the binaries are downgraded.
    - Since the feature is enabled during the 'start' and 'prepare' phases of setFCV, if it creates
      incompatible user data such that setFCV could fail with `CannotUpgrade` or `CannotDowngrade`,
      it can trap the user in a transitional FCV without being able to neither complete the
      upgrade/downgrade without manual intervention nor return to the original FCV.
    - See [SERVER-105672](https://jira.mongodb.org/browse/SERVER-105672) and
      [SERVER-102169](https://jira.mongodb.org/browse/SERVER-102169) for further discussion.
  - If you enable this property, the feature flag description must contain the text '(Enable on
    transitional FCV): ' followed by a justification for why the use of the property is safe.
- check_against_fcv:
  (`operation_fcv_or_fcv_snapshot`|`operation_fcv_only`|`legacy_fcv_snapshot_only`)

  - Optional. Can only be specified for FCV-gated feature flags (`fcv_gated: true`).

    Specifies whether an [Operation FCV](#operation-fcv) (`VersionContext`), a snapshot of the
    server FCV (`FCVSnapshot`), or either, can be used to check if the feature flag is enabled. This
    supports gradual migration of feature flag checks to Operation FCV.

    Each value generates a C++ type with a different signature for feature flag checks. Default
    value is `operation_fcv_or_fcv_snapshot`.

  - `operation_fcv_or_fcv_snapshot`: Allow checking the feature flag either against an Operation FCV
    or an snapshot of the server FCV. The Operation FCV is used if present, but otherwise it falls
    back to the server FCV.
    - Checked as: `gFeatureFlagToaster.isEnabled(VersionContext, FCVSnapshot)`.
  - `operation_fcv_only`: Only allow checking the feature flag only against Operation FCV. If all
    users of this feature flag support Operation FCV, this can be used to enforce it, as well as not
    having to manually acquire and pass in snapshots of the server FCV.
    - Checked as: `gFeatureFlagToaster.isEnabled(VersionContext)`.
  - `legacy_fcv_snapshot_only` for feature flags that have not yet been adapted to the new feature
    flag API introduced in [SERVER-99351](https://jira.mongodb.org/browse/SERVER-99351). Those
    feature flags can be checked without considering Operation FCV. Do not set on new flags.
    - Checked as: `gFeatureFlagToaster.isEnabled(FCVSnapshot)`.

To turn on a feature flag for testing when starting up a server, we would use the following command
line (for the Toaster feature):

```
build/install/bin/mongod --setParameter featureFlagToaster=true
```

However, if you are using resmoke, you will want to turn on the feature flag like so:

```
buildscripts/resmoke.py run  jstests/<path-to-test> --additionalFeatureFlags featureFlagToaster
```

Feature flags should be colocated with the server parameters for the subsystem they are supporting.
For example, a new index build feature that is being flag-guarded may be declared in
`src/mongo/db/storage/two_phase_index_build_knobs.idl`.

## Feature Flag Gating

Each style of feature flag has its own API.

### Binary compatible feature flag API (`BinaryCompatibleFeatureFlag`)

Check the state of a binary compatible feature flag using the `isEnabled()` method:

```c++
if (feature_flags::featureFlagFork.isEnabled()) {
    // The feature flag is enabled. Implement the new behavior.
} else {
    // The feature flag is disabled. Implement the backwards-compatible behavior.
}
```

### IFR feature flag API (`IncrementalRolloutFeatureFlag`)

IFR feature flags provide a `checkEnabled()` method that is similar to
`BinaryCompatibleFeatureFlag::isEnabled()` but also updates a count of how many operations queried
the feature flag, which can be useful diagnostically.

Avoid invoking an IFR flag's `checkEnabled()` method more than once from the same operation, because
the result may not stay the same between checks. Instead, check the flag once and store the result.
Consider using the `IncrementalFeatureRolloutContext` to handle this requirement.

A feature whose behavior applies to a query can use the `IncrementalFeatureRolloutContext` belonging
to the query's `ExpressionContext` for this purpose.

```c++
if (expCtx->getIfrContext()->getSavedFlagValue(featureFlagSpork)) {
    // The feature flag is enabled. Implement the new behavior.
} else {
    // The feature flag is disabled. Implement the backwards-compatible behavior.
}
```

> 🚧 Caution A query can have multiple `ExpressionContext`s if it includes operations with
> subordinate queries, such as the `$lookup` and `$union` pipeline stages, and they do not share the
> same `IncrementalFeatureRolloutContext`.

### FCV-gated feature flag API (`FCVGatedFeatureFlag`)

FCV-gated feature flags can be checked with their `isEnabled()` method:

```c++
if (feature_flags::gFeatureFlagToaster.isEnabled(
        VersionContext::getDecoration(opCtx),
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
    // The feature flag is enabled and we are guaranteed to be only communicating to
    // nodes with binary versions greater than or equal to 4.9.0. Perform the new
    // feature behavior.
} else {
    // It is possible that the feature flag is disabled or we are talking to nodes
    // with binary versions less than 4.9.0. Perform behavior that is compatible with
    // older versions instead.
}
```

For FCV-gated feature flags, a common pattern is to check if the FCV is initialized AND if the
feature flag is enabled on the FCV. In this case, we must make sure to do these checks on the SAME
`FCVSnapshot`:

```c++
const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
if (fcvSnapshot.isVersionInitialized() &&
    feature_flags::gFeatureFlagToaster.isEnabled(VersionContext::getDecoration(opCtx),
                                                  fcvSnapshot)) {
}
```

This is because otherwise, if you use two different snapshots, the in-memory FCV may be
uninitialized in between calling `isVersionInitialized` on the first snapshot, and checking
`isEnabled` on the second snapshot, resulting in a race. This same principle applies in general. If
you want to check multiple properties of the FCV/feature flag at a specific point in time (i.e. you
are expecting the FCV value to be the same in all of your function calls), you must do the checks on
the SAME `FCVSnapshot`. See the
[section about checking the in-memory FCV for more information](README_fcv_overview.md#checking-the-in-memory-fcv)

Furthermore, FCV-gated feature flag checks _must_ pass the
[`VersionContext`](/src/mongo/db/version_context.h) decoration associated with the
`OperationContext`. This requires propagating the `OperationContext`/`VersionContext` all the way
from the beginning of the operation to the point of the feature flag check. In the unusual case
where a feature flag is checked outside of an operation (e.g. during startup), the
`kNoVersionContext` constant should be passed.

See also: [README_version_context.md](/src/mongo/db/README_version_context.md)

This does not apply to feature flags where `fcv_gated` is set to false, since `isEnabled` always
returns the same value after it is initialized during startup.

If the registration of a command should be gated by a feature flag, use `requiresFeatureFlag` when
registering it with `MONGO_REGISTER_COMMAND`. For example:

```c++
MONGO_REGISTER_COMMAND(CommandOnlyEnabledWithToaster)
    .requiresFeatureFlag(mongo::gFeatureFlagToaster);
```

### FCV-gated Feature Flag Behavior During Initial Sync

**_IMPORTANT NOTE ABOUT INITIAL SYNC_**: `isEnabled` checks if the feature flag is enabled on the
input FCV, which is usually the server's current FCV `serverGlobalParams.featureCompatibility`.
However, during initial sync, we temporarily reset the FCV to be uninitialized.

**_`isEnabled` will invariant if the FCV is uninitialized._** Because of this, each feature team
should think about whether the feature could be run during initial sync, for example:

- if the feature is part of initial sync itself
- if the feature is in a background thread that runs during initial sync
- if the feature is run in a command that is allowed during initial sync, such as `hello`,
  `serverStatus`,etc, or any command that returns `secondaryAllowed() == kAlways` or `kOptIn`, and
  returns `maintenanceOk() == true`

If the feature will never run during initial sync, it's fine to continue using `isEnabled`. However,
if the feature could be run during initial sync, the feature team should use one of these options
instead:

- Use `isEnabledUseLastLTSFCVWhenUninitialized`. This checks against the default last LTS FCV
  version if the FCV version is unset, but note that this could result in the feature not being
  turned on even though the FCV will be set to latest once initial sync is complete.
- Use `isEnabledUseLatestFCVWhenUninitialized`. This instead checks against the latest FCV version
  if the FCV version is unset, but note that this could result in the feature being turned on even
  though the FCV has not been upgraded yet and will be set to lastLTS once initial sync is complete.
- Write your own special logic to avoid the invariant. If there is a request for creating additional
  server-wide helper functions in this area, please reach out to the Catalog and Routing team.

### Additional FCV-gated feature flag guidelines

There are some cases outside of startup where we also want to check if the feature flag is turned
on, regardless of which FCV we are on. In these cases we can use the `isEnabledAndIgnoreFCVUnsafe`
helper, but it should only be used when we are sure that we don't care what the FCV is. We should
not use the `isEnabledAndIgnoreFCVUnsafe` helper otherwise because it can result in unsafe scenarios
where we enable a feature on an FCV where it is not supported or where the feature has not been
fully implemented yet. In order to use isEnabledAndIgnoreFCVUnsafe, you **must** add a comment above
that line starting with "(Ignore FCV check):" describing why we can safely ignore checking the FCV
here.

**_[Recall that in a single operation](#dynamically-toggling-features), you must only check the
feature flag once_**. This is because if you checked if the feature flag was enabled multiple times
within a single operation, it's possible that the feature flag and FCV might have become
enabled/disabled during that time, which would result in only part of the operation being executed.
For more details see [SERVER-88965](https://jira.mongodb.org/browse/SERVER-88965) and its linked
issues.

### Operation FCV

There is an ongoing effort to provide operations a stable view of FCV and FCV-gated feature flags.
Instead of acquiring short-lived snapshots of the in-memory FCV (`FCVSnapshot`), a single snapshot
(_Operation FCV_) is acquired and held in the `VersionContext` decoration of the `OperationContext`.

Under this model, the rules for feature flag checks are simplified as follows:

- Operations can check feature flags multiple times, obtaining the same result.
- Operations can check feature flags during oplog application.
  - The Operation FCV is replicated in the oplog, so secondaries apply the op using the same FCV
    snapshot as the primary did ([SERVER-102965](https://jira.mongodb.org/browse/SERVER-102965)).
- Local operations can check feature flags without holding the global lock, and reuse the result of
  that check across multiple global lock acquisitions.
  - setFCV waits for `OperationContext`s with an old Operation FCV to complete before cleaning up
    Server metadata ([SERVER-111447](https://jira.mongodb.org/browse/SERVER-111447)).
- For distributed operations, the result can be reused across multiple `OperationContext`s.
  - setFCV waits for `ShardingCoordinator`s with an old Operation FCV to complete before the config
    server coordinates Server metadata cleanup
    ([SERVER-101537](https://jira.mongodb.org/browse/SERVER-101537)).

Operation FCV is currently only used by coordinators on a sharded cluster (`ShardingCoordinator`)
and by some replica set DDLs (e.g. `create`), but it's intended to eventually extend to all
operations.

### Feature Flag Gating in Tests

In C++ tests, you can use the following RAII guard to ensure that a feature flag is enabled/disabled
when testing a specific codepath:

```c++
// featureFlagToaster has the default value.
{
    unittest::ServerParameterGuard controller("featureFlagToaster", true);
    // Test things that should only happen when featureFlagToaster is enabled.
}
// featureFlagToaster has the default value.
```

In JavaScript, we can use the FeatureFlagUtil library to query the setting for a feature flag:

```c++
if (FeatureFlagUtil.isPresentAndEnabled(db, "Toaster")) {
}
```

### Feature Flag Test Tagging

- When a feature flag is still disabled by default, we can still test the feature flag, as it will
  be enabled by default for testing purposes on the Evergreen variants marked as "all feature
  flags".

- If you have a JS test that depends on this feature flag being enabled, tag with a tag of the same
  name as the feature flag in the format `featureFlagXX` (for example, `featureFlagToaster`). This
  ensures that the test will only be run on the "all feature flags" variants where the feature flag
  is enabled. This works by virtue of the feature flag being declared in the codebase with
  `default: false` which will cause resmoke to ignore tests tagged with the feature flag unless they
  are enabled through the input arguments. _Parallel test suite does not honor feature flag tags,
  due to which some tests may be unexpectedly run in non-feature-flag build variants. If you want to
  skip such tests in parallel suite, please add them to the exclusion list
  [here](https://github.com/mongodb/mongo/blob/eb75b6ccc62f7c8ea26a57c1b5eb96a41809396a/jstests/libs/parallelTester.js#L149)._
  Additionally, the featureFlagXX tags aren't considered by the jstests/core/selinux.js test. If you
  want to skip a test, please use the `no_selinux` tag.
- If you have a JS test that is incompatible with a feature flag being enabled, tag with a tag of
  the same name as the feature flag appended with `_incompatible`in the format
  `featureFlagXX_incompatible` (for example, `featureFlagToaster_incompatible`). This ensures that
  the test will only be run when the selected feature flag is disabled.
- Alternatively, it is fine to check the state of the feature flag in a test and skip the test if
  the feature flag is not enabled (through `FeatureFlagUtil.isEnabled` or using the command
  `getParameter` to query the setting for a feature flag)

- Additionally, if your JS test has multiversion or FCV concerns (for example, if it is run in
  multiversion suites such as `replica_sets_multiversion` and `sharding_multiversion`), **_you
  should also tag the test with the appropriate_**
  [multiversion test tags](https://github.com/mongodb/mongo/blob/a7a2c901882367a8e4a34a97b38acafe07a45566/buildscripts/evergreen_gen_multiversion_tests.py#L55).
  This ensures that the test will not be run in any incompatible multiversion configurations.  
   _For example, if your test depends on a feature that is only enabled in 6.1, tag the test with
  `requires_fcv_61`._ If your test should not be run in any multiversion configurations, tag it with
  `multiversion_incompatible`
- Once the feature flag is enabled by default, you should remove the `featureFlagXX` tag from the
  test. However, you must keep the `requires_fcv_yy` tags.
  - Tests in explicit multiversion suites (e.g. tests in `jstests/multiVersion`) must not carry a
    `requires_fcv_yy` tag at all, since they run in no other suite and the tag would exclude them
    from every run. Check the binary version inside the test instead.
- Test configurations that are incompatible with the feature enabled should have a comment next to
  the tag describing exactly why the test is incompatible to minimize the chance of test coverage
  gaps.

- If a feature flag needs to be disabled on an "all feature flags" build variant, you can add it to
  the escape hatch here: buildscripts/resmokeconfig/fully_disabled_feature_flags.yml
  ([master branch link](/buildscripts/resmokeconfig/fully_disabled_feature_flags.yml)).
- It is not advisable to override the server parameter in the test (e.g. through the nodeOptions
  parameter of a ReplSetTest configuration) because this will make it inconvenient to control the
  feature flag through the CI configuration.

- Feature flags can be individually enabled or disabled using `resmoke.py`'s
  `--additionalFeatureFlags` and `--disableFeatureFlags` arguments, each of which can be specified
  multiple times.

- resmoke.py suite YAMLs that set feature flags on fixtures will not override the options at the
  build variant level. E.g. tests tagged with a certain feature flag will continue to be excluded
  from build variants that don't run with feature flags.

- The feature flag **_MUST NOT_** be enabled outside of the variants that test flag-guarded features
  while the feature is in development or while the feature flag is still disabled by default. This
  is necessary so that the Release team can be certain that failures outside of those variants
  indicate bugs that will be released in the next version.

- Before enabling a feature flag by default, check through your open build failures to see if you
  have any failures on the feature flag buildvariant which should be fixed first.

- If you want to test upgrade/downgrade behavior for an FCV-gated feature flag, please refer to
  [this test](/jstests/multiVersion/genericBinVersion/example_fcv_upgrade_downgrade_test.js) as an
  example.

See also: [README_multiversion_testing.md](README_multiversion_testing.md) for an overview of
multiversion and upgrade/downgrade testing in general.
