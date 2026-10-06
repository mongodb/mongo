# Feature Compatibility Version

> Part of the [FCV and Feature Flag documentation](README.md#fcv-and-feature-flags).

Feature compatibility version (FCV) is the versioning mechanism for a MongoDB cluster that provides
safety guarantees when upgrading and downgrading between versions. The FCV determines the version of
the feature set exposed by the cluster and is often set in lockstep with the binary version as a
part of
[upgrading or downgrading the cluster's binary version](https://docs.mongodb.com/v5.0/release-notes/5.0-upgrade-replica-set/#upgrade-a-replica-set-to-5.0).

FCV is used to disable features that may be problematic when active in a mixed version cluster. For
example, incompatibility issues can arise if a newer version node accepts an instance of a new
feature _f_ while there are still older version nodes in the cluster that are unable to handle _f_.

FCV is persisted as a document in the `admin.system.version` collection. It will look something like
the following if a node were to be in FCV 5.0:

```
   { "_id" : "featureCompatibilityVersion", "version" : "5.0" }
```

This document is present in every mongod in the cluster and is replicated to other members of the
replica set whenever it is updated via writes to the `admin.system.version` collection. The FCV
document is also present on standalone nodes.

## FCV on Startup

On a clean startup (the server currently has no replicated collections), the server will
[create the FCV document for the first time](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/startup_recovery.cpp#L619).
If it is running as a shard server (with the `--shardsvr option`), the server will
[set the FCV to be the last LTS version](https://github.com/mongodb/mongo/blob/386b1c0c74aa24c306f0ef5bcbde892aec89c8f6/src/mongo/db/commands/feature_compatibility_version.cpp#L442).
This is to ensure compatibility when adding the shard to a downgraded version cluster. The config
server will run `setFeatureCompatibilityVersion` on the shard to match the clusters FCV as part of
`addShard`. If the server is not running as a shard server, then the server will set its FCV to the
latest version by default.

As part of a startup with an existing FCV document, the server caches an in-memory value of the FCV
from disk. The `FcvOpObserver` keeps this in-memory value in sync with the on-disk FCV document
whenever an update to the document is made. In the period of time during startup where the in-memory
value has yet to be loaded from disk, the FCV is set to `kUnsetDefaultLastLTSBehavior`. This
indicates that the server will be using the last-LTS feature set as to ensure compatibility with
other nodes in the replica set.

As part of initial sync, the in-memory FCV value is always initially set to be
`kUnsetDefaultLastLTSBehavior`. This is to ensure compatibility between the sync source and sync
target. If the sync source is actually in a different feature compatibility version, we will find
out when we clone the `admin.system.version` collection. However, since we can't guarantee that we
will clone the `admin.system.version` collection first, we first
[manually set our in-memory FCV value to match the sync source's FCV](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/repl/initial_syncer.cpp#L1142-L1146).
We won't persist the FCV on disk nor will we update our minWireVersion until we clone the actual
document, but this in-memory FCV value will ensure that we clone collections using the same FCV as
the sync source.

A node that starts with `--replSet` will also have an FCV value of `kUnsetDefaultLastLTSBehavior` if
it has not yet received the `replSetInitiate` command.

## Checking the in memory FCV

The in-memory FCV can be accessed through
`serverGlobalParams.featureCompatibility.acquireFCVSnapshot()`. This gets an immutable _copy_ of the
current FCV, so that we can use multiple functions like `getVersion` or `isVersionInitialized` to
check a snapshot of the FCV value, without that value changing or getting reset (such as during
initial sync) in-between the function calls. This means that if we want to check multiple properties
of what the FCV is at a particular point in time, we should do it all on the same snapshot.

For example: checking `isVersionInitialized() && isLessThan()` with the same `FCVSnapshot` value
would have the guarantee that if the FCV value is initialized during `isVersionInitialized()`, it
will still be during `isLessThan()`.

Conversely, if we do set the in-memory FCV to another value through
`serverGlobalParams.mutableFCV.setVersion`, and we want to now check something on the new FCV, we
need to make sure to re-call `acquireFCVSnapshot` to get a new copy of the FCV.

In general, if you want to check multiple properties of the FCV at a specific point in time, you
should use one snapshot. For example, if you want to check both that the FCV is initialized, and if
it's less than some version, and that featureFlagXX is enabled on this FCV, this should all be using
the same `FCVSnapshot`. But if you're doing a multiple completely separate FCV checks at different
points in time, such as over multiple functions,or multiple distinct feature flag enablement checks
(i.e. `featureFlagXX.isEnabled && featureFlagYY.isEnabled`), you should acquire a new FCV snapshot
since the old one may be stale.

## Checking FCV in a JS Test

Sometimes JS tests need to branch on FCV, for example if the test runs in multiversion suites and
exercises behavior that differs depending on a node's FCV.

You can accomplish this by using `getParameter` to obtain the FCV. You should also file a SERVER
ticket and add a TODO as a reminder to remove the FCV check when it is no longer needed in the
future.

```js
// TODO SERVER-XXXXX: Remove branching once last-lts FCV is >= X.Y.
const fcvDoc = db.adminCommand({
  getParameter: 1,
  featureCompatibilityVersion: 1,
});
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "X.Y") >= 0) {
  // code here for FCV >= X.Y
} else {
  // code here for FCV < X.Y
}
```

See also: [README_setfcv_command.md](README_setfcv_command.md)
