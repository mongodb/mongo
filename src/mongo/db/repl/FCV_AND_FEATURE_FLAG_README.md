# Feature Compatibility Version

Feature compatibility version (FCV) is the versioning mechanism for a MongoDB cluster that provides
safety guarantees when upgrading and downgrading between versions. The FCV determines the version of
the feature set exposed by the cluster and is often set in lockstep with the binary version as a
part of [upgrading or downgrading the cluster's binary version](https://docs.mongodb.com/v5.0/release-notes/5.0-upgrade-replica-set/#upgrade-a-replica-set-to-5.0).

FCV is used to disable features that may be problematic when active in a mixed version cluster.
For example, incompatibility issues can arise if a newer version node accepts an instance of a new
feature *f* while there are still older version nodes in the cluster that are unable to handle
*f*.

FCV is persisted as a document in the `admin.system.version` collection. It will look something like
the following if a node were to be in FCV 5.0:
<pre><code>
   { "_id" : "featureCompatibilityVersion", "version" : "5.0" }</code></pre>

This document is present in every mongod in the cluster and is replicated to other members of the
replica set whenever it is updated via writes to the `admin.system.version` collection. The FCV
document is also present on standalone nodes.

## FCV on Startup

On a clean startup (the server currently has no replicated collections), the server will [create the FCV document for the first time](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/startup_recovery.cpp#L619). 
If it is running as a shard server (with the `--shardsvr option`),
the server will [set the FCV to be the last LTS version](https://github.com/10gen/mongo/blob/386b1c0c74aa24c306f0ef5bcbde892aec89c8f6/src/mongo/db/commands/feature_compatibility_version.cpp#L442). 
This is to ensure compatibility when adding
the shard to a downgraded version cluster. The config server will run
`setFeatureCompatibilityVersion`on the shard to match the clusters FCV as part of `addShard`. If the
server is not running as a shard server, then the server will set its FCV to the latest version by
default.

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
will clone the `admin.system.version` collection first, we first [manually set our in-memory FCV value to match the sync source's FCV](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/repl/initial_syncer.cpp#L1142-L1146).
We won't persist the FCV on disk nor will we update our minWireVersion until we clone the actual
document, but this in-memory FCV value will ensure that we clone collections using the same FCV as
the sync source.

A node that starts with `--replSet` will also have an FCV value of `kUnsetDefaultLastLTSBehavior`
if it has not yet received the `replSetInitiate` command.

# setFeatureCompatibilityVersion Command Overview

The FCV can be set using the `setFeatureCompatibilityVersion` admin command to one of the following:
* The version of the last-LTS (Long Term Support) 
  * Indicates to the server to use the feature set compatible with the last LTS release version.
* The version of the last-continuous release
  * Indicates to the server to use the feature set compatible with the last continuous release
version.
* The version of the latest(current) release
  * Indicates to the server to use the feature set compatible with the latest release version.
In a replica set configuration, this command should be run against the primary node. In a sharded
configuration this should be run against the mongos. The mongos will forward the command
to the config servers which then forward request again to shard primaries. As mongos nodes are
non-data bearing, they do not have an FCV.

Each `mongod` release will support the following upgrade/downgrade paths:
* Last-Continuous → Latest
    * Note that we do not support downgrading to or from Last-Continuous.
* Last-LTS ←→ Latest
* Last-LTS → Last-Continuous
  * This upgrade-only transition is only possible when requested by the [config server](https://docs.mongodb.com/manual/core/sharded-cluster-config-servers/).
  * Additionally, the last LTS must not be equal to the last continuous release.

The command also requires a `{confirm: true}` parameter. This is so that users acknowledge that an 
FCV + binary downgrade will require support assistance. Without this parameter, the 
setFeatureCompatibilityVersion command for downgrade will [error](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L290-L298)
and say that once they have downgraded the FCV, if you choose to downgrade the binary version, it
will require support assistance. Similarly, the setFeatureCompatibilityVersion command for upgrade
will also error and say that once the cluster is upgraded, FCV + binary downgrade will no longer be
possible without support assistance.

As part of an upgrade/downgrade, the FCV will transition through these states:
<pre><code>
Upgrade:
   kVersion_X → kUpgradingFrom_X_To_Y → kVersion_Y

Downgrade:
   kVersion_X → kDowngradingFrom_X_To_Y → isCleaningServerMetadata → kVersion_Y
</code></pre>
In above, X will be the source version that we are upgrading/downgrading from while Y is the target
version that we are upgrading/downgrading to.

These are the steps that the setFCV command goes through. See [adding code to the setFCV command](#adding-upgradedowngrade-related-code-to-the-setfcv-command) 
for more information on how to add upgrade/downgrade code to the command. 

1. **Transition to `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y`**

    * In the first part, we start transition to `requestedVersion` by [updating the local FCV document to a
`kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y` state](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L430-L437), respectively.

    * Transitioning to one of the `kUpgradingFrom_X_To_Y`/`kDowngradingFrom_X_To_Y` states updates
the FCV document in `admin.system.version` with a new `targetVersion` field. Transitioning to a
`kDowngradingFrom_X_to_Y` state in particular will also add a `previousVersion` field along with the
`targetVersion` field. These updates are done with `writeConcern: majority`. 

    * Transitioning to one of the `kUpgradingFrom_X_To_Y`/`kDowngradingFrom_X_to_Y`/`kVersion_Y`(on
upgrade) states [sets the `minWireVersion` to `WireVersion::LATEST_WIRE_VERSION`](https://github.com/10gen/mongo/blob/386b1c0c74aa24c306f0ef5bcbde892aec89c8f6/src/mongo/db/op_observer/fcv_op_observer.cpp#L69)
and also [closes all incoming connections from internal clients with lower binary versions](https://github.com/10gen/mongo/blob/386b1c0c74aa24c306f0ef5bcbde892aec89c8f6/src/mongo/db/op_observer/fcv_op_observer.cpp#L76-L82).

    * **This step is expected to be fast and always succeed** (except if the request parameters fail validation
    e.g. if the requested FCV is not a valid transition).

Some examples of on-disk representations of the upgrading and downgrading states:
<pre><code>
kUpgradingFrom_5_0_To_5_1:
{
    version: 5.0,
    targetVersion: 5.1
}

kDowngradingFrom_5_1_To_5_0:
{ 
    version: 5.0, 
    targetVersion: 5.0,
    previousVersion: 5.1
}
</code></pre>


2. **Run [`_prepareToUpgrade` or `_prepareToDowngrade`](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L497-L501):** 
    * First, we do any actions to prepare for upgrade/downgrade that must be taken before the FCV
 full transition lock. For example, we cancel serverless migrations in this step. 
    * Then, the FCV full transition lock is acquired in shared
mode and then released immediately. This creates a barrier and guarantees safety for operations
that acquire the global lock either in exclusive or intent exclusive mode. If these operations begin
and acquire the global lock prior to the FCV change, they will proceed in the context of the old
FCV, and will guarantee to finish before the FCV change takes place. For the operations that begin
after the FCV change, they will see the updated FCV and behave accordingly. This also means that
in order to make this barrier truly safe, **in any given operation, we should only check the 
feature flag/FCV after acquiring the appropriate locks**. See the [section about setFCV locks](#setfcv-locks)
for more information on the locks used in the setFCV command.
    * Finally, we check for any user data or settings that will be incompatible on 
 the new FCV, and uassert with the `CannotUpgrade` or `CannotDowngrade` code if the user needs to manually clean up 
 incompatible user data. This is especially important on downgrade. 
    * If an FCV downgrade fails at this point, the user can either remove the incompatible user data and retry the FCV downgrade, or they can upgrade the FCV back to the original FCV.
    * On this part no metadata cleanup is performed yet.

3. **Complete any [upgrade or downgrade specific code](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L524-L528), done in `_runUpgrade` or `_runDowngrade`.** This may include metadata cleanup. 
    * For upgrade, we update metadata to make sure the new features in the upgraded version work for 
 both sharded and non-sharded clusters. 
    * For downgrade, we transition from `kDowngradingFrom_X_to_Y` to 
`isCleaningServerMetadata`, which indicates that we have started [cleaning up internal server metadata](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1495).  Transitioning to 
`isCleaningServerMetadata` will add a `isCleaningServerMetadata` field, which will be removed upon
transitioning to `kVersion_Y`. This update is also done using `writeConcern: majority`.
After this point, if the FCV downgrade fails, it is no longer safe to transition back to the original 
upgraded FCV, and the user must retry the FCV downgrade. Then we perform any internal server downgrade cleanup.

Examples on-disk representation of the `isCleaningServerMetadata` state:
<pre><code>
isCleaningServerMetadata after kDowngradingFrom_5_1_To_5_0:
{ 
    version: 5.0, 
    targetVersion: 5.0,
    previousVersion: 5.1,
    isCleaningServerMetadata: true
}
</code></pre>

4. Finally, we [complete transition](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L541-L548) by updating the
local FCV document to the fully upgraded or downgraded version. As part of transitioning to the 
`kVersion_Y` state, the `targetVersion`, `previousVersion`, and `isCleaningServerMetadata` 
(if applicable) fields of the FCV document are deleted while the `version` field is updated to 
reflect the new upgraded or downgraded state. This update is also done using `writeConcern: majority`.
The new in-memory FCV value will be updated to reflect the on-disk changes.

    * Note that for an FCV upgrade, we do an extra step to run `_finalizeUpgrade` **after** updating
    the FCV document to fully upgraded. This is for any tasks that cannot be done until after the 
    FCV is fully upgraded, because during `_runUpgrade`, the FCV is still in the transitional state
    (which behaves like the downgraded FCV)

## The SetFCV Command on Sharded Clusters
On a sharded cluster, the command is driven by the config server. The config server runs a 3-phase
protocol for updating the FCV on the cluster. Shard servers will go through all the steps outlined
above (please read the [setFeatureCompatibilityVersion Command Overview section](#setFeatureCompatibilityVersion-Command-Overview)), 
but will be explicitly told when to do each step by the config servers. Config servers go through 
the phases in lock step with the shard servers to make sure that they are always on the same phase 
or one phase ahead of shard servers. For example, the config server cannot be in phase 3 if any 
shard server is still in phase 1. 

Additionally, when the config server sends each command to each of
the shards, this is done [synchronously](https://github.com/10gen/mongo/blob/1c97952f194d80e0ba58a4fbe553f09326a5407f/src/mongo/db/s/config/sharding_catalog_manager.cpp#L858-L887), so the config will send the command to one shard and wait for 
either a success or failure response. If it succeeds, then the config server will send the 
command to the next shard. If it fails, then the whole FCV upgrade/downgrade will [fail](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1032-L1033). This means that if one shard succeeds but another fails, the overall FCV upgrade/downgrade
will fail.

1. First, the config server transitions to `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y` (shards are still in the 
old FCV). 
2. Phase-1
    * a. Config server [sends phase-1 command to shards](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L476).
    * b. Shard servers transition  to `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y`.
    * c. Shard servers do any [phase-1 tasks](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L460) (for downgrading, this would include stopping new features).
3. Phase-2 (throughout this phase config and shards are all in the transitional FCV)
    * a. Config server runs `_prepareToUpgrade` or `_prepareToDowngrade`, takes the full FCV transition lock, 
    and verifies user data compatibility for upgrade/downgrade. 
    * b. Config server [sends phase-2 command to shards](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L506-L507). 
    * c. Shard servers run `_prepareToUpgrade` or `_prepareToDowngrade`, takes the full FCV transition lock, 
    and verifies user data compatibility for upgrade/downgrade. 
4. Phase-3
    * a. Config server runs `_runUpgrade` or `_runDowngrade`. For downgrade, this means the config
    server enters the `isCleaningServerMetadata` phase and cleans up any internal server metadata.
    * b. Config server [sends phase-3 command to shards](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1499).
    * c. Shard servers run `_runUpgrade` or `_runDowngrade`. For downgrade, this means the shard
    servers enter the `isCleaningServerMetadata` phase and cleans up any internal server metadata.
    * d. Shards finish and enter the fully upgraded or downgraded state (on upgrade, the config 
    server would still be in the `kUpgradingFrom_X_To_Y` phase, and on downgrade the config server
    would still be in the `isCleaningServerMetadata` phase).
    * e. Config finishes and enters the fully upgraded or downgraded state.

Note that on downgrade, if the setFCV command fails at any point between 4a and 4e, the user will 
not be able to transition back to the original upgraded FCV, since either the config server and/or
the shard servers are in the middle of cleaning up internal server metadata.

## SetFCV Command Errors
The setFCV command can only fail with these error cases: 
* Retryable error (such as `InterruptedDueToReplStateChange`)
    * The user must retry the FCV upgrade/downgrade, so the code must be idempotent and retryable.
* `CannotDowngrade`: 
    * The user can either remove the incompatible user data and retry the FCV downgrade, or they can upgrade the FCV back to the original FCV.
    * Because of this, the code in the upgrade path must be able to work if started from any point in the 
    transitional `kDowngradingFrom_X_To_Y` state. 
    * The code in the FCV downgrade path must be idempotent and retryable.
* `CannotUpgrade`: 
    * The user would need to fix the incompatible user data and retry the FCV upgrade.
* Other `uasserts`:
    * For example, if the user attempted to upgrade the FCV after the previous FCV downgrade failed
    during `isCleaningServerMetadata`. In this case the user would need to retry the FCV downgrade.
* `ManualInterventionRequired` or `fassert`: 
    * `ManualInterventionRequired` indicates a server bug
    but that all the data is consistent on disk and for reads/writes, and an `fassert`
    indicates a server bug and that the data is corrupted. 
    * `ManualInterventionRequired`
    and `fasserts` are errors that should not occur in practice, but if they did,
    they would turn into a Support case.

## SetFCV Locks
There are three locks used in the setFCV command:
* [setFCVCommandLock](https://github.com/mongodb/mongo/blob/eb5d4ed00d889306f061428f5652431301feba8e/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L294)
    * This ensures that only one invocation of the setFCV command can run at a time (i.e. if you 
    ran setFCV twice in a row, the second invocation would not run until the first had completed)
* [fcvDocumentLock](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/commands/feature_compatibility_version.cpp#L215) 
    * The setFCV command takes this lock in X mode when it modifies the FCV document. This includes
    from [fully upgraded -> downgrading](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L350), 
    [downgrading -> isCleaningServerMetadata](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1459-L1460),
    [isCleaningServerMetadata -> fully downgraded](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L533),
    and vice versa. 
    * Other operations should [take this lock in shared mode](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/commands/feature_compatibility_version.cpp#L594-L599)
    if they want to ensure that the FCV state _does not change at all_ during the operation. 
    See [example](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/s/config/sharding_catalog_manager_collection_operations.cpp#L489-L490)
* [FCV full transition lock](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/concurrency/lock_manager_defs.h#L326)
    * The setFCV command [takes this lock in S mode and then releases it immediately](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L515-L525)
    after we are in the upgrading/downgrading state,
    but before we transition from the upgrading/downgrading state to the fully upgraded/downgraded 
    state.
    * The lock creates a barrier for operations taking the global IX or X locks, which implicitly 
    take the FCV full transition lock in IX mode (aside from those which explicitly opt out). 
    * This is to ensure that the FCV does not _fully_ transition between the upgraded and downgraded
    versions (or vice versa) during these other operations. This is because either:
        * The global IX/X locked operation will start after the FCV change, see the 
        upgrading/downgrading to the new FCV and act accordingly.
        * The global IX/X locked operation began prior to the FCV change. The operation will proceed
        in the context of the old FCV, and will guarantee to finish before upgrade/downgrade 
        procedures begin right after this barrier
    * This also means that in order to make this barrier truly safe, if we want to ensure that the
    FCV does not change during our operation, **you must take the global IX or X lock first, and 
    then check the feature flag/FCV value after that point**
    * Other operations that take the global IX or X locks already conflict with the FCV full 
    transition lock by default, unless [_shouldConflictWithSetFeatureCompatibilityVersion](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/concurrency/locker.h#L489-L495)
    is specifically set to false. This should only be set to false in very special cases.

_Code spelunking starting points:_
* [The template file used to generate the FCV constants](https://github.com/mongodb/mongo/blob/c4d2ed3292b0e113135dd85185c27a8235ea1814/src/mongo/util/version/releases.h.tpl#L1)
* [The `FCVTransitions` class, that determines valid FCV transitions](https://github.com/mongodb/mongo/blob/c4d2ed3292b0e113135dd85185c27a8235ea1814/src/mongo/db/commands/feature_compatibility_version.cpp#L75)


## Adding upgrade/downgrade related code to the setFCV command 
The `setFeatureCompatibilityVersion` command is done in three parts. This corresponds to the different
states that the FCV document can be in, as described in the above section.

In the first part, we start transition to `requestedVersion` by [updating the local FCV document to a
`kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y` state](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L430-L437), respectively. 
**This step is expected to be fast and always succeed.** This means that code that
might fail or take a long time should ***not*** be added before this point in the 
`setFeatureCompatibilityVersion` command.

In the second part, we perform [upgrade/downgrade-ability checks](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L497-L501). This is done on `_prepareToUpgrade`
and `_prepareToDowngrade`. On this part no metadata cleanup is performed yet.

In the last part, we complete any [upgrade or downgrade specific code](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L524-L528), done in `_runUpgrade` and 
`_runDowngrade`. This includes possible metadata cleanup. Note that once we start `_runDowngrade`, 
we cannot transition back to `kUpgradingFrom_X_To_Y`until the full downgrade completes.

Then we [complete transition](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L541-L548) by updating the
local FCV document to the fully upgraded or downgraded version.

***All feature-specific FCV upgrade or downgrade code should go into the following functions.*** 

`_shardServerPhase1Tasks`: This helper function is only for any actions that should be done specifically on
shard servers during phase 1 of the 3-phase setFCV protocol for sharded clusters.
For example, before completing phase 1, we must wait for backward incompatible
ShardingDDLCoordinators to finish. This is important in order to ensure that no
shard that is currently a participant of such a backward-incompatible
ShardingDDLCoordinator can transition to the fully downgraded state (and thus,
possibly downgrade its binary) while the coordinator is still in progress.
The fact that the FCV has already transitioned to kDowngrading ensures that no
new backward-incompatible ShardingDDLCoordinators can start.
We do not expect any other feature-specific work to be done in the 'start' phase.

`_prepareToUpgrade`  performs all actions and checks that need to be done before proceeding to make 
any metadata changes as part of FCV upgrade. Any new feature specific upgrade code should be placed 
in the helper functions:
* `_prepareToUpgradeActions`: for any upgrade actions that should be done before taking the FCV full 
transition lock in S mode. It is required that the code in this helper function is
idempotent and could be done after `_runDowngrade` even if `_runDowngrade` failed at any point.
* `_userCollectionsWorkForUpgrade`: for any user collections uasserts (with the `CannotUpgrade` error code),
creations, or deletions that need to happen during the upgrade. This happens after the FCV full
transition lock.  It is required that the code in this helper function is idempotent and could be
done after `_runDowngrade` even if `_runDowngrade` failed at any point.

`_runUpgrade`: _runUpgrade performs all the metadata-changing actions of an FCV upgrade. Any new 
feature specific upgrade code should be placed in the `_runUpgrade` helper functions: 
* `_upgradeServerMetadata`: for updating server metadata to make sure the new features in the upgraded version 
work for sharded and non-sharded clusters. It is required that the code in this helper function is
idempotent and could be done after `_runDowngrade` even if `_runDowngrade` failed at any point.


`_finalizeUpgrade`: only for any tasks that must be done to fully complete the FCV upgrade
AFTER the FCV document has already been updated to the UPGRADED FCV.
This is because during `_runUpgrade`, the FCV is still in the transitional state (which behaves
like the downgraded FCV), so certain tasks cannot be done yet until the FCV is fully
upgraded.

Additionally, it's possible that during an FCV upgrade, the replset/shard server/config server
undergoes failover AFTER the FCV document has already been updated to the UPGRADED FCV, but
before the cluster has completed `_finalizeUpgrade`. In this case, since the cluster failed over,
the user/client may retry sending the setFCV command to the cluster, but the cluster is 
already in the requestedVersion (i.e. `requestedVersion == actualVersion`). However,
the cluster should retry/complete the tasks from `_finalizeUpgrade` before sending ok:1
back to the user/client. Therefore, these tasks **must** be idempotent/retryable.

`_prepareToDowngrade` performs all actions and checks that need to be done before proceeding to make 
any metadata changes as part of FCV downgrade. Any new feature specific downgrade code should be 
placed in the helper functions:
* `_prepareToDowngradeActions`: Any downgrade actions that should be done before taking the FCV full 
transition lock in S mode should go in this function.
* `_userCollectionsUassertsForDowngrade`: for any checks on user data or settings that will uassert
with the `CannotDowngrade` code if users need to manually clean up user data or settings.

`_runDowngrade:` _runDowngrade performs all the metadata-changing actions of an FCV downgrade. Any 
new feature specific downgrade code should be placed in the `_runDowngrade` helper functions: 
* `_internalServerCleanupForDowngrade`: for any internal server downgrade cleanup. Any code in this
function is required to be *idempotent* and *retryable* in case the node crashes or downgrade fails in a
way that the user has to run setFCV again. It cannot fail for a non-retryable reason since at this
point user data has already been cleaned up. It also must be able to be *rolled back*. This is 
because we cannot guarantee the safety of any server metadata that is not replicated in the event of
a rollback. 
    * This function can only fail with some transient error that can be retried
    (like `InterruptedDueToReplStateChange`), `ManualInterventionRequired`, or `fasserts`. For
    any non-retryable error in this helper function, it should error either with an
    uassert with `ManualInterventionRequired` as the error code (indicating a server bug
    but that all the data is consistent on disk and for reads/writes) or with an `fassert`
    (indicating a server bug and that the data is corrupted). `ManualInterventionRequired`
    and `fasserts` are errors that are not expected to occur in practice, but if they did,
    they would turn into a Support case.


One common pattern for FCV downgrade is to check whether a feature needs to be cleaned up on 
downgrade because it is not enabled on the downgraded version. For example, if we are on 6.1 and are
downgrading to 6.0, we must check if there are any new features that may have been used that are not
enabled on 6.0, and perform any necessary downgrade logic for that. 

To do so, we must do the following ([example in the codebase](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1061-L1063)): 

```
if (!featureFlag.isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
 // do feature specific checks/downgrade logic
}
```
where `requestedVersion` is the version we are downgrading to and `originalVersion` is the version
we are downgrading from. 

Similarly, we can use [isEnabledOnTargetFCVButDisabledOnOriginalFCV](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L809-L810)
for upgrade checks. 

```
if (!featureFlag.isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, originalVersion)) {
 // do feature specific checks/upgrade logic
}
```
See the [feature flags](#feature-flags) section for more information on feature 
flags.

# Generic FCV references
Sometimes, we may want to make a generic FCV reference to implement logic around upgrade/downgrade 
that is not specific to a certain release version.

For these checks, we *must* use the [generic constants](https://github.com/mongodb/mongo/blob/e08eba28ab9ad4d54adb95e8517c9d43276e5336/src/mongo/db/server_options.h#L202-L216). 
We should not be using the FCV constants like kVersion_6_0 ([example of what to avoid](https://github.com/10gen/mongo/blob/ef8bdb8d0cbd584d47c54d64c3215ae29ec1a32f/src/mongo/db/pipeline/document_source_list_catalog.cpp#L130)). 
Instead, we should branch 
the different behavior using feature flags (see [When to Use Feature Flags](#when-to-use-feature-flags) and [Feature Flag Gating](#feature-flag-gating)).
For generic cases
that only need to check if the server is currently in the middle of an upgrade/downgrade, use the 
[isUpgradingOrDowngrading()](https://github.com/mongodb/mongo/blob/e08eba28ab9ad4d54adb95e8517c9d43276e5336/src/mongo/db/server_options.h#L275-L281) helper.

Example:
The server includes [logic to check](https://github.com/mongodb/mongo/blob/d3fedc03bb3b2037bc4f2266b4cd106377c217b7/src/mongo/db/fcv_op_observer.cpp#L58-L71) 
that connections from internal clients with a lower binary version are closed whenever we set a new 
FCV. This logic is expected to stay across LTS releases as it is not specific to a particular 
release version.

***Using these generic constants and helpers indicates to the Replication team that the FCV logic 
should not be removed after the next LTS release.***

## Linter Rule
To avoid misuse of these generic FCV constants and to make sure all generic FCV references are 
indeed meant to exist across LTS binary versions, ***a comment containing “(Generic FCV reference):” 
is required within 10 lines before a generic FCV reference.*** See [this example](https://github.com/mongodb/mongo/blob/24890bbac9ee27cf3fb9a1b6bb8123ab120a1594/src/mongo/db/s/config/sharding_catalog_manager_shard_operations.cpp#L341-L347).
([SERVER-49520](https://jira.mongodb.org/browse/SERVER-49520) added a linter rule for this.)

# FCV Constants Generation
The FCV constants for each mongo version are not hardcoded in the code base but are dynamically
generated instead. We do this to make upgrading the FCV constants easier after every release. The
constants are generated at compile time, using the [latest git tag](https://github.com/mongodb/mongo/tags)
alongside a [list of versions](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.yml).

The git tag and `releases.yml` file are used as inputs to a [template file](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.h.tpl),
which the build infrastructure uses to generate a `releases.h` file that contains our constants.
Please see a sample `releases.h` file generated when latest is 7.0 [here](https://gist.github.com/XueruiFa/afc40c9ffe30049e61378af8724c86bc).

The logic for determining our generic FCVs is:

* Latest: The FCV in the [`featureCompatibilityVersions` list](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.yml#L7)
in `releases.yml` that is equal to the git tag.
* Last Continuous: The highest FCV in `featureCompatibilityVersions` that is less than latest FCV.
* Last LTS: The highest FCV in the [`longTermSupportReleases` list](https://github.com/mongodb/mongo/blob/96ea1942d25bfc6b2ab30779590f1b8a8c6887b5/src/mongo/util/version/releases.yml#L25)
in `releases.yml` that is less than latest FCV.

## Branch Cut and Upgrading FCVs
Since the FCV generation logic is entirely dependent on the git tag, the Server Triage and Release
(STAR) team will upgrade the git tag on the master branch after every release. When this happens,
to correctly build mongo after every release, developers will need to pull the new git tag.

This can be done by using the `--tags` option (i.e., running `git fetch --tags`) after the STAR
team has introduced the new git tag. Developers may also see what their latest git tag is by
running `git describe`. After fetching the latest git tag, it will be necessary to recompile so
that the new `releases.h` file can be generated.

# Feature Flags

## What are Feature Flags
Feature flags are a technique to support new server features that are still in active development on
the master branch. This technique allows us to iteratively commit code for a new server feature
without the risk of accidentally enabling the feature for a scheduled product release.

The criteria for determining whether a feature flag can be enabled by default and released is
largely specific to the scope and design but ideally a feature can be made available when:

* There is sufficient test coverage to provide confidence that the feature works as designed.
* Upgrade/downgrade issues have been addressed.
* The feature does not destabilize the server as a whole while running in our CI system.

## When to use Feature Flags

Feature flags are a requirement for continuous delivery, thus features that are in development must
have a feature flag. Features that span multiple commits should also have a feature flag associated 
with them because continuous delivery means that we often branch in the middle of feature 
development.

Additionally, any project or ticket that wants to introduce different behavior based on which FCV
the server is running ***must*** add a feature flag. In the past, the branching of the different
behavior would be done by directly checking which FCV the server was running. However, we now must 
***not*** be using any references to FCV constants such as kVersion_6_0 ([example of what to avoid](https://github.com/10gen/mongo/blob/ef8bdb8d0cbd584d47c54d64c3215ae29ec1a32f/src/mongo/db/pipeline/document_source_list_catalog.cpp#L130)).
Instead we should branch 
the different behavior using feature flags (see [Feature Flag Gating](#feature-flag-gating)).
***This means that individual ticket that wants to introduce an FCV check will also need to create a 
feature flag specific to that ticket.***

The motivation for using feature flags rather than checking FCV constants directly is because
checking FCV constants directly is more error prone and has caused issues in the release process
when updating/removing outdated FCV constants. 

Note that ***we do not support disabling feature flags once they have been enabled via IDL in a release build***. 
Therefore, feature flags should ***not*** be used for parameters that will be turned on and off. Our 
entire feature flag system is built on the assumption that these are used for preventing 
in-development code from being exposed to users, and not for turning off arbitrary features after 
they've been released.

## Lifecycle of a feature flag
* Adding the feature flag
    * Disabled by default. This minimizes disruption to the CI system and BB process.
    * This should be done concurrently with the first work ticket in the PM for the feature.
* Enabling the feature by default
    * Feature flags with default:true must have a specific release version associated in its 
    definition.
    * ***We do not support disabling feature flags once they have been enabled via IDL in a release
    build***.
    * Project team should run a full patch build against all the ! builders to minimize the impact
    on the Build Baron process.
    * If there are downstream teams that will break when this flag is enabled then enabling the
    feature by default will need to wait until those teams have affirmed they have adapted their 
    product.
    * JS tests tagged with this feature flag should remove the `featureFlagXX` tag. The test should 
    add a `requires_fcv_yy` tag to ensure that the test will not be run in incompatible multiversion
    configurations. (The `requires_fcv_yy` tag is enforced as of [SERVER-55858](https://jira.mongodb.org/browse/SERVER-55858)). 
    See the [Feature Flag Test Tagging](#feature-flag-test-tagging) section for more details. 
    * After this point the feature flag is used for FCV gating to make upgrade/downgrade safe.

* Removing the feature flag
    * Any projects/tickets that use and enable a feature flag ***must*** leave that feature flag in
    the codebase at least until the next major release. 
        * For example, if a feature flag was enabled by default in 5.1, it must remain in the 
        codebase until 6.0 is branched. After that, the feature flag can be removed. 
        * This is because the feature flag is used for FCV gating during the upgrade/downgrade 
        process. For example, if a feature is completed and the feature flag is enabled by default 
        in FCV 5.1, then from binary versions 5.1, 5.2, 5.3, and 6.0, the server could have its FCV 
        set to 5.0 during the downgrade process, where the feature is not supposed to be enabled. If
        the feature flag was removed earlier than 6.0 then the feature would still run upon 
        downgrading the FCV to 5.0.


## Creating a Feature Flag

Feature flags are created by adding it to an IDL file:

```
// featureFlagToaster is a feature flag that is under development and off by default.
// Enabling this feature flag will associate it with the latest FCV (eg. 4.9.0).
   featureFlagToaster:
     description: "Create a feature flag"
     cpp_varname: gFeatureFlagToaster
     default: false
```

A feature flag has the following properties:

* Server Parameter Name: featureFlag<MyFeatureName>
    * The name should not include "Use", "Allow", "Enable", or "Disable".
* Server Parameter set_at value: [ startup ]
    * Feature flags should generally be settable at server startup only. This supports the use of
    feature flags as a release/development tool and should not impose an unnecessary burden on
    server administration.
* C++ Type: mongo::feature_flags
    * This should apply to all feature flags that follow the naming convention in the previous
    bullet.
    * No other type should be allowed for feature flags. For server parameters that support
    multiple enumerated choices (for example, protocol/implementation for a command), these should
    be declared separately. Feature flags may still be used to toggle support for a particular
    option for the non-feature flag server parameter.
* C++ Name: gFeatureFlagToaster
* Default Value: false
    * The default value for a new feature flag should always start with false.
    * When the feature is fully tested and ready for release, we can change the default to true.
    At that time, a "version" must be specified.
* Version: string - a string for the FCV version
    * Required field if default is true, Must be a string acceptable to 
    FeatureCompatibilityVersionParser.

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

To check if a feature flag is enabled, we should do the following: 
```
if(feature_flags::gFeatureFlagToaster.isEnabled(serverGlobalParams.featureCompatibility)) {
	// The feature flag is enabled and we are guaranteed to be only communicating to
    // nodes with binary versions greater than or equal to 4.9.0. Perform the new 
    // feature behavior.
} else {
	// It is possible that the feature flag is disabled or we are talking to nodes
    // with binary versions less than 4.9.0. Perform behavior that is compatible with
    // older versions instead.
}
```

Note that this assumes that `serverGlobalParams.featureCompatibility` has already been initialized. 
If we are calling `isEnabled(serverGlobalParams.featureCompatibility)` in a location where it might
not already be initialized, we must do this instead:

```
if(serverGlobalParams.featureCompatibility.isVersionInitialized() && 
feature_flags::gFeatureFlagToaster.isEnabled(serverGlobalParams.featureCompatibility)) {
	// code if feature is enabled.
} else {
	// code if feature is not enabled.
}
```
or we can use the [`isEnabledUseDefaultFCVWhenUninitialized` helper](https://github.com/10gen/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/feature_flag.cpp#L67).
```
if(feature_flags::gFeatureFlagToaster.isEnabledUseDefaultFCVWhenUninitialized(serverGlobalParams.featureCompatibility)) {
	// code if feature is enabled on current FCV if initialized or if feature is enabled on lastLTS FCV.
} else {
	// code if feature is not enabled.
}
```
This returns true if the flag is set to true and enabled for this FCV version. If the FCV version is
unset, it instead checks against the default last LTS FCV version.

There are some places where we only want to check if the feature flag is turned on, regardless of
which FCV we are on. For example, this could be the case if we need to perform the check in a spot
in the code when the FCV has not been initialized yet during startup. In these cases we should use the 
`isEnabledAndIgnoreFCVUnsafeAtStartup` helper.

There are some cases outside of startup where we also want to check if the feature flag is turned on,
regardless of which FCV we are on. In these cases we can use the `isEnabledAndIgnoreFCVUnsafe`
helper, but it should only be used when we are sure that we don't care what the FCV is. We should 
not use the `isEnabledAndIgnoreFCVUnsafe` helper otherwise because it can result in unsafe scenarios
where we enable a feature on an FCV where it is not supported or where the feature has not been 
fully implemented yet. In order to use isEnabledAndIgnoreFCVUnsafe, you **must** add a comment above
that line starting with "(Ignore FCV check):" describing why we can safely ignore checking the FCV
here. 

***Note that in a single operation, you must only check the feature flag once***. This is because if
you checked if the feature flag was enabled multiple times within a single operation, it's possible 
that the feature flag and FCV might have become enabled/disabled during that time, which would 
result in only part of the operation being executed. 


### Feature Flag Gating in Tests
In C++ tests, you can use the following RAII guard to ensure that a feature flag is enabled/disabled
when testing a specific codepath:
```
// featureFlagToaster has the default value.
{
    RAIIServerParameterControllerForTest controller("featureFlagToaster", true);
    // Test things that should only happen when featureFlagToaster is enabled.
}
// featureFlagToaster has the default value.
```

In JavaScript, we can use the command getParameter to query the setting for a feature flag:
```
const isToasterEnabled = db.adminCommand({getParameter: 1, featureFlagToaster: 1}).featureFlagToaster.value;
```
We can also use the FeatureFlagUtil library like so: 
```
if (FeatureFlagUtil.isEnabled(db, "Toaster")) {
}
```

### Feature Flag Test Tagging
* When a feature flag is still disabled by default, we can still test the feature flag, as it will 
be enabled by default for testing purposes on the Evergreen variants marked as "all feature flags". 

* If you have a JS test that depends on this feature flag being enabled, tag with a tag of the same 
name as the feature flag in the format `featureFlagXX` (for example, `featureFlagToaster`). This 
ensures that the test will only be run on the "all feature flags" variants where the feature flag is
enabled. This works by virtue of the feature flag being declared in the codebase without
`default: true` which will cause it to make its way into the all_feature_flags.txt file used by
resmoke.py to control either which flags to enable for "all feature flags", or which tags
(e.g. featureFlagXX) to skip when it is not running "all feature flags."
    * Parallel test suite does not honor feature flag tags, due to which some tests may be 
    unexpectedly run in non-feature-flag build variants. If you want to skip such tests in 
    parallel suite, please add them to the exclusion list [here](https://github.com/mongodb/mongo/blob/eb75b6ccc62f7c8ea26a57c1b5eb96a41809396a/jstests/libs/parallelTester.js#L149).
    * Additionally, the featureFlagXX tags aren't considered by the jstests/core/selinux.js test.
    If you want to skip a test, please use the `no_selinux` tag. 
* Alternatively, it is fine to check the state of the feature flag in a test and skip the test
if the feature flag is not enabled (through `FeatureFlagUtil.isEnabled` or using the command 
`getParameter` to query the setting for a feature flag)

* Additionally, if your JS test has multiversion or FCV concerns (for example, if it is run in 
multiversion suites such as `replica_sets_multiversion` and `sharding_multiversion`), ***you should
also tag the test with the appropriate*** [multiversion test tags](https://github.com/mongodb/mongo/blob/a7a2c901882367a8e4a34a97b38acafe07a45566/buildscripts/evergreen_gen_multiversion_tests.py#L55).
This ensures that the test will not be run in any incompatible multiversion configurations.  
    * For example, if your test depends on a feature that is only enabled in 6.1, tag the test
    with `requires_fcv_61`. 
    * If your test should not be run in any multiversion configurations, tag it with 
    `multiversion_incompatible`
    
* Once the feature flag is enabled by default, you should remove the `featureFlagXX` tag from the
test. However, you must keep the `requires_fcv_yy` tags. 
    
* Test configurations that are incompatible with the feature enabled should have a comment next 
to the tag describing exactly why the test is incompatible to minimize the chance of test 
coverage gaps.

* If a feature flag needs to be disabled on an "all feature flags" build variant, you can add it
to the escape hatch here: buildscripts/resmokeconfig/fully_disabled_feature_flags.yml 
([master branch link](https://github.com/mongodb/mongo/blob/master/buildscripts/resmokeconfig/fully_disabled_feature_flags.yml)).
    
* It is not advisable to override the server parameter in the test (e.g. through the nodeOptions
parameter of a ReplSetTest configuration) because this will make it inconvenient to control the 
feature flag through the CI configuration.

* --additionalFeatureFlags can be passed into resmoke.py to force enable certain feature flags. 
The option is additive and can be specified multiple times.

* resmoke.py suite YAMLs that set feature flags on fixtures will not override the options at the
build variant level. E.g. tests tagged with a certain feature flag will continue to be excluded 
from build variants that don't run with feature flags.

* The feature flag ***MUST NOT*** be enabled outside of the variants that test flag-guarded features
while the feature is in development or while the feature flag is still disabled by default. This
is necessary so that the Release team can be certain that failures outside of those variants 
indicate bugs that will be released in the next version.

* Before enabling a feature flag by default, check through your open build failures to see if you 
have any failures on the feature flag buildvariant which should be fixed first.

# Summary of Rules for FCV and Feature Flag Usage:
* Any project or ticket that wants to introduce different behavior based on which FCV
the server is running ***must*** add a feature flag. 
* We should ***not*** be adding any references to FCV constants such as kVersion_6_0.
* To make sure all generic FCV references are 
indeed meant to exist across LTS binary versions, ***a comment containing “(Generic FCV reference):” 
is required within 10 lines before a generic FCV reference.***
* If you want to ensure that the FCV will not change during your operation, you must take the global
IX or X lock first, and then check the feature flag/FCV value after that point.
*  In a single operation, you should only check the feature flag once, as reads are non-repeatable.
* Any projects/tickets that use and enable a feature flag ***must*** leave that feature flag in the 
codebase at least until the next major release. 
* We do not support disabling feature flags once they have been enabled via IDL in a release build.
* In general, tag tests that depend on a feature flag with `featureFlagXX` and `requires_fcv_yy`
tags, where `yy` is the FCV that the feature flag is/will be enabled by default on.
