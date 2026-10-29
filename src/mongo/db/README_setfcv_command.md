# setFeatureCompatibilityVersion Command Overview

> Part of the [FCV and Feature Flag documentation](README.md#fcv-and-feature-flags). See also:
> [README_fcv_overview.md](README_fcv_overview.md).

The FCV can be set using the `setFeatureCompatibilityVersion` admin command to one of the following:

- The version of the last-LTS (Long Term Support)
  - Indicates to the server to use the feature set compatible with the last LTS release version.
- The version of the last-continuous release
  - Indicates to the server to use the feature set compatible with the last continuous release
    version.
- The version of the latest(current) release
  - Indicates to the server to use the feature set compatible with the latest release version. In a
    replica set configuration, this command should be run against the primary node. In a sharded
    configuration this should be run against the mongos. The mongos will forward the command to the
    config servers which then forward request again to shard primaries. As mongos nodes are non-data
    bearing, they do not have an FCV.

Each `mongod` release will support the following upgrade/downgrade paths:

- Last-Continuous ←→ Latest
- Last-LTS ←→ Latest
- Last-LTS → Last-Continuous
  - This upgrade-only transition is only possible when requested by the
    [config server](https://docs.mongodb.com/manual/core/sharded-cluster-config-servers/).
  - Additionally, the last LTS must not be equal to the last continuous release.

The command also requires a `{confirm: true}` parameter. This is so that users acknowledge that an
FCV + binary downgrade will require support assistance. Without this parameter, the
setFeatureCompatibilityVersion command for downgrade will
[error](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L290-L298)
and say that once they have downgraded the FCV, if you choose to downgrade the binary version, it
will require support assistance. Similarly, the setFeatureCompatibilityVersion command for upgrade
will also error and say that once the cluster is upgraded, FCV + binary downgrade will no longer be
possible without support assistance.

As part of an upgrade/downgrade, the FCV will transition through these states:

```
Upgrade:   kVersion_X → kUpgradingFrom_X_To_Y   → kUpgradingFrom_X_To_Y   (isCleaningServerMetadata: true) → kVersion_Y
Downgrade: kVersion_X → kDowngradingFrom_X_To_Y → kDowngradingFrom_X_To_Y (isCleaningServerMetadata: true) → kVersion_Y
```

In above, X will be the source version that we are upgrading/downgrading from while Y is the target
version that we are upgrading/downgrading to.

These are the steps that the setFCV command goes through. See
[adding code to the setFCV command](#adding-upgradedowngrade-related-code-to-the-setfcv-command) for
more information on how to add upgrade/downgrade code to the command.

1.  **Transition to `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y`**

    - In the first part, we start transition to `requestedVersion` by
      [updating the local FCV document to a `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y` state](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L430-L437),
      respectively.

    - Transitioning to one of the `kUpgradingFrom_X_To_Y`/`kDowngradingFrom_X_To_Y` states updates
      the FCV document in `admin.system.version` with a new `targetVersion` field. Transitioning to
      a `kDowngradingFrom_X_to_Y` state in particular will also add a `previousVersion` field along
      with the `targetVersion` field. These updates are done with `writeConcern: majority`.

    - Transitioning to one of the `kUpgradingFrom_X_To_Y`/`kDowngradingFrom_X_to_Y`/`kVersion_Y`(on
      upgrade) states
      [sets the `minWireVersion` to `WireVersion::LATEST_WIRE_VERSION`](https://github.com/mongodb/mongo/blob/386b1c0c74aa24c306f0ef5bcbde892aec89c8f6/src/mongo/db/op_observer/fcv_op_observer.cpp#L69)
      and also
      [closes all incoming connections from internal clients with lower binary versions](https://github.com/mongodb/mongo/blob/386b1c0c74aa24c306f0ef5bcbde892aec89c8f6/src/mongo/db/op_observer/fcv_op_observer.cpp#L76-L82).
      The reason we do this on `kDowngradingFrom_X_to_Y` is because we shouldn’t decrease the
      minWireVersion until we have fully downgraded to the lower FCV in case we get any backwards
      compatibility breakages, since during `kDowngradingFrom_X_to_Y` we may still be
      stopping/cleaning up any features from the upgraded FCV. In essence, a node with the upgraded
      FCV/binary should not be able to communicate with downgraded binary nodes until the FCV is
      completely downgraded to `kVersion_Y`.

    - **This step is expected to be fast and always succeed** (except if the request parameters fail
      validation e.g. if the requested FCV is not a valid transition).

      Some examples of on-disk representations of the upgrading and downgrading states:

      ```
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
      ```

2.  **Run
    [`_prepareToUpgrade` or `_prepareToDowngrade`](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L497-L501):**

    - First, we do any actions to prepare for upgrade/downgrade that must be taken before the global
      lock. For example, we cancel serverless migrations in this step.
    - Then, the global lock is acquired in shared mode and then released immediately. This creates a
      barrier and guarantees safety for operations that acquire the global lock either in exclusive
      or intent exclusive mode. If these operations begin and acquire the global lock prior to the
      FCV change, they may proceed in the context of the old FCV, but setFCV waits for them to
      finish before proceeding to the metadata cleanup. For the operations that begin after the FCV
      change, they will see the updated FCV and behave accordingly. This also means that in order to
      make this barrier truly safe, **in any given operation, we should only check the feature
      flag/FCV after acquiring the appropriate locks**. See the
      [section about setFCV locks](#setfcv-locks) for more information on the locks used in the
      setFCV command.
    - Finally, we check for any user data or settings that will be incompatible on the new FCV, and
      uassert with the `CannotUpgrade` or `CannotDowngrade` code if the user needs to manually clean
      up incompatible user data.
    - If an FCV downgrade fails at this point, the user can either remove the incompatible user data
      and retry the FCV downgrade, or they can upgrade the FCV back to the original FCV.
    - Similarly, if an FCV upgrade fails at this point, the user can either remove the incompatible
      user data (i.e. stop using discontinued features) and retry the FCV upgrade, or they can
      downgrade the FCV back to the original FCV.
    - On this part no metadata cleanup is performed yet.

3.  **Set the
    [`isCleaningServerMetadata` flag](https://github.com/mongodb/mongo/blob/0afb563897d4db79fe50e81b07d2384e1d05a939/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L801-L827):**

    - This indicates that we have started
      [cleaning up internal server metadata](https://github.com/mongodb/mongo/blob/0afb563897d4db79fe50e81b07d2384e1d05a939/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L829-L837)
      to be consistent with the persisted formats of the new FCV.

      The `isCleaningServerMetadata` flag is added as a new field to the FCV document, which will
      later be removed upon transitioning to `kVersion_Y`. This update is also done using
      `writeConcern: majority`.

      After this point, if the FCV upgrade or downgrade is interrupted, it is no longer safe to
      transition back to the original FCV, and the user can retry the transition until it succeeds.
      Any non-transient failure after this point indicates a server bug.

      Examples on-disk representation of the `isCleaningServerMetadata` state:

      ```
      isCleaningServerMetadata after kDowngradingFrom_5_1_To_5_0:
      {
          version: 5.0,
          targetVersion: 5.0,
          previousVersion: 5.1,
          isCleaningServerMetadata: true
      }
      ```

4.  **Complete any
    [upgrade or downgrade specific code](https://github.com/mongodb/mongo/blob/0afb563897d4db79fe50e81b07d2384e1d05a939/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L829-L837),
    done in `_runUpgrade` or `_runDowngrade`.**

    - For downgrade, this may include cleaning up any metadata from the upgraded FCV that is not
      needed in the downgraded FCV.

    - For upgrade, we update metadata to make sure the new features in the upgraded version work for
      both sharded and non-sharded clusters.

5.  Finally, we
    [complete transition](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L541-L548)
    by updating the local FCV document to the fully upgraded or downgraded version. As part of
    transitioning to the `kVersion_Y` state, the `targetVersion`, `previousVersion`, and
    `isCleaningServerMetadata` fields of the FCV document are deleted while the `version` field is
    updated to reflect the new upgraded or downgraded state. This update is also done using
    `writeConcern: majority`. The new in-memory FCV value will be updated to reflect the on-disk
    changes.

    - After the transition, the global lock is again acquired in shared mode and then released
      immediately. This creates a barrier for operations that acquire the global lock either in
      exclusive or intent exclusive mode, and guarantees that we wait until operations that may run
      on the context of the old FCV to finish before we proceed with the tasks that can only be done
      after the FCV is fully upgraded/downgraded.

    - Note that for an FCV upgrade, we do an extra step to run `_finalizeUpgrade` **after** updating
      the FCV document to fully upgraded. This is for any tasks that cannot be done until after the
      FCV is fully upgraded, because during `_runUpgrade`, the FCV is still in the transitional
      state (which behaves like the downgraded FCV)

## The SetFCV Command on Sharded Clusters

On a sharded cluster, the command is driven by the config server. The config server runs a 3-phase
protocol for updating the FCV on the cluster. Shard servers will go through all the steps outlined
above (please read the
[setFeatureCompatibilityVersion Command Overview section above](#setfeaturecompatibilityversion-command-overview)),
but will be explicitly told when to do each step by the config servers. Config servers go through
the phases in lock step with the shard servers to make sure that they are always on the same phase
or one phase ahead of shard servers. For example, the config server cannot be in phase 3 if any
shard server is still in phase 1.

Additionally, when the config server sends each command to each of the shards, this is done
[synchronously](https://github.com/mongodb/mongo/blob/1c97952f194d80e0ba58a4fbe553f09326a5407f/src/mongo/db/s/config/sharding_catalog_manager.cpp#L858-L887),
so the config will send the command to one shard and wait for either a success or failure response.
If it succeeds, then the config server will send the command to the next shard. If it fails, then
the whole FCV upgrade/downgrade will
[fail](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1032-L1033).
This means that if one shard succeeds but another fails, the overall FCV upgrade/downgrade will
fail.

1. First, the config server transitions to `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y`
   (shards are still in the old FCV).
2. Phase-1
   - a. Config server
     [sends phase-1 command to shards](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L476).
   - b. Shard servers transition to `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y`.
   - c. Shard servers do any
     [phase-1 tasks](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L460)
     (for downgrading, this would include stopping new features).
3. Phase-2 (throughout this phase config and shards are all in the transitional FCV)
   - a. Config server runs `_prepareToUpgrade` or `_prepareToDowngrade`, takes the global lock, and
     verifies user data compatibility for upgrade/downgrade.
   - b. Config server
     [sends phase-2 command to shards](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L506-L507).
   - c. Shard servers run `_prepareToUpgrade` or `_prepareToDowngrade`, takes the global lock, and
     verifies user data compatibility for upgrade/downgrade.
4. Phase-3
   - a. Config server runs `_runUpgrade` or `_runDowngrade`. This means the config server sets the
     `isCleaningServerMetadata` flag and cleans up any internal server metadata.
   - b. Config server
     [sends phase-3 command to shards](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1499).
   - c. Shard servers run `_runUpgrade` or `_runDowngrade`. This means the shard servers sets the
     `isCleaningServerMetadata` flag and cleans up any internal server metadata.
   - d. Shards finish and enter the fully upgraded or downgraded state (the config server is still
     in the transitional upgrade/downgrading phase, with the `isCleaningServerMetadata` flag set).
   - e. Config finishes and enters the fully upgraded or downgraded state.

Note that if the setFCV command fails at any point between 4a and 4e, the user will not be able to
transition back to the original upgraded FCV, since either the config server and/or the shard
servers are in the middle of cleaning up internal server metadata.

## Dry-run mode for setFCV Command

The setFCV command supports a dry-run mode to detect uses of forwards or backwards incompatible
features without operational impact nor changes to persisted data. This mode performs validations
and reports for the use of forwards incompatible features (during upgrade) or backwards incompatible
features (during downgrade) to the user, informing them that the upgrade or downgrade will not
succeed. The dry-run check is exposed in two complementary ways:

- As an explicit user request, by including the parameter `dryRun: true`, which allows validating
  the use of incompatible features at any time, without performing the actual FCV upgrade/downgrade.
  ```
  db.runCommand({
    setFeatureCompatibilityVersion: '9.0',
    dryRun: true
  })
  ```
- Automatically before an actual FCV upgrade/downgrade, to prevent the transition from starting if
  any use of incompatible features is detected.
  - For sharded clusters, the config server coordinates the dry-run mode across the shards after its
    own dry-run has succeeded.

If a setFCV command fails during dry-run mode, a `CannotUpgrade` or `CannotDowngrade` error will be
thrown and the transition won't start, preventing the user from leaving the FCV in a transitional
state.

### Guidelines for adding new compatibility checks

The upgrade/downgrade compatibility checks are done in the functions
`_userCollectionsUassertsForUpgrade` and `_userCollectionsUassertsForDowngrade`, respectively. These
functions are read-only (i.e. must not modify any user data or system state), so that their only
purpose is to check for unmet preconditions and fail with a `CannotUpgrade` or `CannotDowngrade`
error if the user needs to manually clean up data or settings before proceeding. Thus, every added
check should be placed in one of these functions and must adhere to the following principles:

- **Fast and non-disruptive**: Checks must complete quickly (under 10 seconds in typical scenarios)
  and have no noticeable impact on performance. This ensures users can perform a dry-run validation
  on a live system without disruption.
  - In particular, avoid scanning large datasets like user data or chunks. It may be acceptable,
    however, to inspect collection/index metadata through the in-memory collection catalog.
- **Report downstream impact**: When adding a new feasibility check, remember to fill the
  appropriate project/ticket metadata to notify any impact to other teams and document any manual
  upgrade/downgrade instructions.

### Skipping the automatic dry-run

For advanced use cases, you can bypass the automatic dry-run validation by including the
`skipDryRun: true` parameter.

```
db.runCommand({
  setFeatureCompatibilityVersion: '9.0',
  confirm: true,
  skipDryRun: true
})
```

**Warning**: When this parameter is used and incompatible user data exists, the setFCV command will
not fail before the actual FCV transition. Instead, it will begin the process of transitioning and
then fail, leaving the FCV in a transitional "upgrading" or "downgrading" state.

## SetFCV Command Errors

The setFCV command can only fail with these error cases:

- Retryable error (such as `InterruptedDueToReplStateChange`)
  - The user must retry the FCV upgrade/downgrade, so the code must be idempotent and retryable.
- `CannotDowngrade`:
  - The user can either remove the incompatible user data and retry the FCV downgrade, or they can
    upgrade the FCV back to the original FCV.
  - Because of this, the code in the upgrade path must be able to work if started from any point in
    the transitional `kDowngradingFrom_X_To_Y` state.
  - The code in the FCV downgrade path must be idempotent and retryable.
- `CannotUpgrade`:
  - The user can either remove the incompatible user data (i.e. stop using discontinued features)
    and retry the FCV upgrade, or they can downgrade the FCV back to the original FCV.
  - Because of this, the code in the downgrade path must be able to work if started from any point
    in the transitional `kUpgradingFrom_X_To_Y` state.
  - The code in the FCV upgrade path must be idempotent and retryable.
- Other `uasserts`:
  - For example, if the user attempted to upgrade the FCV after the previous FCV downgrade failed
    during `isCleaningServerMetadata`. In this case the user would need to retry the FCV downgrade.
- `ManualInterventionRequired` or `fassert`:
  - `ManualInterventionRequired` indicates a server bug but that all the data is consistent on disk
    and for reads/writes, and an `fassert` indicates a server bug and that the data is corrupted.
  - `ManualInterventionRequired` and `fasserts` are errors that should not occur in practice, but if
    they did, they would turn into a Support case.

## SetFCV Locks

There are three locks used in the setFCV command:

- [setFCVCommandLock](https://github.com/mongodb/mongo/blob/eb5d4ed00d889306f061428f5652431301feba8e/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L294)
  - This ensures that only one invocation of the setFCV command can run at a time (i.e. if you ran
    setFCV twice in a row, the second invocation would not run until the first had completed)
- [fcvDocumentLock](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/commands/feature_compatibility_version.cpp#L215)
  - The setFCV command takes this lock in X mode when it modifies the FCV document. This is done:
    - From
      [fully downgraded → upgrading, or fully upgraded → downgrading](https://github.com/mongodb/mongo/blob/a87555389a8e5e39f87db6b9b6a9309c56723378/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L635),
    - From
      [upgrading → upgrading (isCleaningServerMetadata: true), or downgrading → downgrading (isCleaningServerMetadata: true)](https://github.com/mongodb/mongo/blob/a87555389a8e5e39f87db6b9b6a9309c56723378/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L812),
    - From
      [upgrading (isCleaningServerMetadata: true) → fully upgraded, or downgrading (isCleaningServerMetadata: true) → fully downgraded](https://github.com/mongodb/mongo/blob/a87555389a8e5e39f87db6b9b6a9309c56723378/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L842).
  - Other operations should
    [take this lock in shared mode](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/commands/feature_compatibility_version.cpp#L594-L599)
    if they want to ensure that the FCV state _does not change at all_ during the operation. See
    [example](https://github.com/mongodb/mongo/blob/bd8a8d4d880577302c777ff961f359b03435126a/src/mongo/db/s/config/sharding_catalog_manager_collection_operations.cpp#L489-L490)
- [Global lock](/src/mongo/db/shard_role/lock_manager/d_concurrency.h)
  - The setFCV command
    [takes this lock in S mode and then releases it immediately](https://github.com/mongodb/mongo/blob/418028cf4dcf416d5ab87552721ed3559bce5507/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L551-L557)
    shortly after the FCV transitions to a new value (either to the upgrading/downgrading state, or
    to the fully upgrade/downgraded state).
  - The lock creates a barrier for operations taking the global IX or X locks.
  - This is to ensure that the FCV does not _fully_ transition between the upgraded and downgraded
    versions (or vice versa) during these other operations. This is because either: _The global IX/X
    locked operation will start after the FCV change, see the new FCV and act accordingly._ The
    global IX/X locked operation began prior to the FCV change. The operation may proceed in the
    context of the old FCV, but setFCV waits for them to finish before upgrade/downgrade metadata
    cleanup procedures begin right after this barrier.
  - This also means that in order to make this barrier truly safe, if we want to ensure that the FCV
    can not go through multiple transitions during our operation, **you must take the global IX or X
    lock first, and then check the feature flag/FCV value after that point**

_Code spelunking starting points:_

- [The template file used to generate the FCV constants](https://github.com/mongodb/mongo/blob/c4d2ed3292b0e113135dd85185c27a8235ea1814/src/mongo/util/version/releases.h.tpl#L1)
- [The `FCVTransitions` class, that determines valid FCV transitions](https://github.com/mongodb/mongo/blob/c4d2ed3292b0e113135dd85185c27a8235ea1814/src/mongo/db/commands/feature_compatibility_version.cpp#L75)

## Adding upgrade/downgrade related code to the setFCV command

The `setFeatureCompatibilityVersion` command is done in three parts. This corresponds to the
different states that the FCV document can be in, as described in the above section.

In the first part, we start transition to `requestedVersion` by
[updating the local FCV document to a `kUpgradingFrom_X_To_Y` or `kDowngradingFrom_X_To_Y` state](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L430-L437),
respectively. **This step is expected to be fast and always succeed.** This means that code that
might fail or take a long time should **_not_** be added before this point in the
`setFeatureCompatibilityVersion` command.

In the second part, we perform
[upgrade/downgrade-ability checks](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L497-L501).
This is done on `_prepareToUpgrade` and `_prepareToDowngrade`. On this part no metadata cleanup is
performed yet.

In the last part, we complete any
[upgrade or downgrade specific code](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L524-L528),
done in `_runUpgrade` and `_runDowngrade`. This includes possible metadata cleanup. Note that once
we start `_runDowngrade`, we cannot transition back to `kUpgradingFrom_X_To_Y`until the full
downgrade completes.

Then we
[complete transition](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L541-L548)
by updating the local FCV document to the fully upgraded or downgraded version.

**_All feature-specific FCV upgrade or downgrade code should go into the following functions._**

`_shardServerPhase1Tasks`: This helper function is only for any actions that should be done
specifically on shard servers during phase 1 of the 3-phase setFCV protocol for sharded clusters.
For example, before completing phase 1, we must wait for backward incompatible ShardingCoordinators
to finish. This is important in order to ensure that no shard that is currently a participant of
such a backward-incompatible ShardingCoordinator can transition to the fully downgraded state (and
thus, possibly downgrade its binary) while the coordinator is still in progress. The fact that the
FCV has already transitioned to kDowngrading ensures that no new backward-incompatible
ShardingCoordinators can start. We do not expect any other feature-specific work to be done in the
'start' phase.

`_prepareToUpgrade` performs all actions and checks that need to be done before proceeding to make
any metadata changes as part of FCV upgrade. Any new feature specific upgrade code should be placed
in the helper functions:

- `_prepareToUpgradeActions`: for any upgrade actions that should be done before taking the global
  lock in S mode. It is required that the code in this helper function is idempotent and could be
  done after `_runDowngrade` even if `_runDowngrade` failed at any point.
- `_userCollectionsUassertsForUpgrade`: for any checks on user data or settings that will uassert
  with the `CannotUpgrade` code if users need to manually clean up user data or settings. It must
  not modify any user data or system state. It only checks preconditions for upgrades and fails if
  they are unmet (see
  [guidelines for adding new compatibility checks](#guidelines-for-adding-new-compatibility-checks)).
- `_userCollectionsWorkForUpgrade`: for any creations, changes or deletions that need to happen
  during the upgrade. This happens after the global lock. It is required that the code in this
  helper function is idempotent and could be done after `_runDowngrade` even if `_runDowngrade`
  failed at any point.

`_runUpgrade`: \_runUpgrade performs all the metadata-changing actions of an FCV upgrade. Any new
feature specific upgrade code should be placed in the `_runUpgrade` helper functions:

- `_upgradeServerMetadata`: for updating server metadata to make sure the new features in the
  upgraded version work for sharded and non-sharded clusters. It is required that the code in this
  helper function is idempotent and could be done after `_runDowngrade` even if `_runDowngrade`
  failed at any point.

`_finalizeUpgrade`: only for any tasks that must be done to fully complete the FCV upgrade AFTER the
FCV document has already been updated to the UPGRADED FCV. This is because during `_runUpgrade`, the
FCV is still in the transitional state (which behaves like the downgraded FCV), so certain tasks
cannot be done yet until the FCV is fully upgraded.

Additionally, it's possible that during an FCV upgrade, the replset/shard server/config server
undergoes failover AFTER the FCV document has already been updated to the UPGRADED FCV, but before
the cluster has completed `_finalizeUpgrade`. In this case, since the cluster failed over, the
user/client may retry sending the setFCV command to the cluster, but the cluster is already in the
requestedVersion (i.e. `requestedVersion == actualVersion`). However, the cluster should
retry/complete the tasks from `_finalizeUpgrade` before sending ok:1 back to the user/client.
Therefore, these tasks **must** be idempotent/retryable.

`_prepareToDowngrade` performs all actions and checks that need to be done before proceeding to make
any metadata changes as part of FCV downgrade. Any new feature specific downgrade code should be
placed in the helper functions:

- `_prepareToDowngradeActions`: Any downgrade actions that should be done before taking the global
  lock in S mode should go in this function.
- `_userCollectionsUassertsForDowngrade`: for any checks on user data or settings that will uassert
  with the `CannotDowngrade` code if users need to manually clean up user data or settings. It must
  not modify any user data or system state. It only checks preconditions for downgrades and fails if
  they are unmet (see
  [guidelines for adding new compatibility checks](#guidelines-for-adding-new-compatibility-checks)).

`_runDowngrade:` \_runDowngrade performs all the metadata-changing actions of an FCV downgrade. Any
new feature specific downgrade code should be placed in the `_runDowngrade` helper functions:

- `_internalServerCleanupForDowngrade`: for any internal server downgrade cleanup. Any code in this
  function is required to be _idempotent_ and _retryable_ in case the node crashes or downgrade
  fails in a way that the user has to run setFCV again. It cannot fail for a non-retryable reason
  since at this point user data has already been cleaned up. It also must be able to be _rolled
  back_. This is because we cannot guarantee the safety of any server metadata that is not
  replicated in the event of a rollback. \* This function can only fail with some transient error
  that can be retried (like `InterruptedDueToReplStateChange`), `ManualInterventionRequired`, or
  `fasserts`. For any non-retryable error in this helper function, it should error either with an
  uassert with `ManualInterventionRequired` as the error code (indicating a server bug but that all
  the data is consistent on disk and for reads/writes) or with an `fassert` (indicating a server bug
  and that the data is corrupted). `ManualInterventionRequired` and `fasserts` are errors that are
  not expected to occur in practice, but if they did, they would turn into a Support case.

One common pattern for FCV downgrade is to check whether a feature needs to be cleaned up on
downgrade because it is not enabled on the downgraded version. For example, if we are on 6.1 and are
downgrading to 6.0, we must check if there are any new features that may have been used that are not
enabled on 6.0, and perform any necessary downgrade logic for that.

To do so, we must do the following
([example in the codebase](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L1061-L1063)):

```
if (!featureFlag.isDisabledOnTargetFCVButEnabledOnOriginalFCV(requestedVersion, originalVersion)) {
 // do feature specific checks/downgrade logic
}
```

where `requestedVersion` is the version we are downgrading to and `originalVersion` is the version
we are downgrading from.

Similarly, we can use
[isEnabledOnTargetFCVButDisabledOnOriginalFCV](https://github.com/mongodb/mongo/blob/c6e5701933a98b4fe91c2409c212fcce2d3d34f0/src/mongo/db/commands/set_feature_compatibility_version_command.cpp#L809-L810)
for upgrade checks.

```
if (!featureFlag.isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, originalVersion)) {
 // do feature specific checks/upgrade logic
}
```

See the [feature flags](README_feature_flags.md) section for more information on feature flags.
