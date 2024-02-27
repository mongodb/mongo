# Serverless Internals

## Shard Split

A shard split is one of the serverless scaling primitives, allowing for scale out by migrating data for one or many tenants from an existing replica set to a newly formed replica set.

The following diagram illustrates the lifetime of a shard split operation:
![shard_split_diagram](../../../../docs/images/shard_split_diagram.png)

### Protocol

A shard is split by calling the `commitShardSplit` command, and is generally issued by a cloud component such as the atlasproxy. The shard split protocol consists of an exchange of messages between two shards: the donor and recipient. This exchange is orchestrated by the donor shard in a PrimaryOnlyService implementation, which has the following steps:

1. **Start the split operation**
   The donor receives a `commitShardSplit` command with a `recipientSetName`, `recipientTagName`, and list of tenants that should be split into the recipient. The `recipientTagName` identifies recipient nodes in the donor config, and the `recipientSetName` is the setName for the recipient replica set.

    All active index builds for collections belonging to tenants which will be split are [aborted](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L649-L652) at the start of the split operation. All index builds for tenants being split will be blocked for the duration of the operation.

    Finally, the donor [reserves an oplog slot](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L926), called the `blockTimestamp`, after which all user requests for tenants being split will be blocked. It then durably records a state document update to the `kBlocking` state at the `blockTimestamp`, and enters the split critical section.

2. **Wait for recipient nodes to catch up**
   Before proceeding with any split-specific steps, the donor must wait for all recipient nodes to catch up to the `blockTimestamp`. This wait is accomplished by calling [ReplicationCoordinator::awaitReplication with a custom tagged writeConcern](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L702), which targets nodes in the local config with the `recipientTagName`. Note that because of how replica set tags are implemented, each recipient node must have a different value for the `recipientTagName` ([learn more](https://www.mongodb.com/docs/manual/tutorial/configure-replica-set-tag-sets/#std-label-configure-custom-write-concern)). Donor nodes are guaranteed to be caught up because we [wait for majority write](https://github.com/mongodb/mongo/blob/c2a1125bc0bb729acfec94a94be924b2bb65d128/src/mongo/db/serverless/shard_split_donor_service.cpp#L663-L667) of the state document establishing the `blockTimestamp`.

3. **Applying the split**
   The donor then [prepares a "split config"](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L718-L730) which is a copy of the current config with recipient nodes removed, an increased version, and a new subdocument (`recipientConfig`) which contains the config recipient nodes will apply during split. The recipient config is a copy of the current config with donor nodes removed, recipient nodes reindexed from zero, a new set name. The donor then calls `replSetReconfig` on itself with the split config.

    Recipient nodes learn of the split config through heartbeats. When a recipient node sees a split config, it will first [wait for its oplog buffers to drain](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L682). This guarantees that the `lastAppliedOpTime` reported by each node in their `hello` responses gives an accurate view of which node is furthest along in application.

    After draining, the recipient node will install the embedded recipient config. Once the config is successfully installed the recipient node will clear its <code>[lastCommittedOpTime and currentCommittedOpTime](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L1065-L1066)</code> and [restart oplog application](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L1068-L1070). We clear these two pieces of metadata to guarantee that recipient nodes never propagate opTimes from the donor timeline.

4. **Accepting the split**
   The donor [creates one SingleServerDiscoveryMonitor per recipient node](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L561) at the beginning of a split operation in order to monitor recipient nodes for split acceptance. The primary criteria for split acceptance is that each recipient node reports the `recipientTagName`, however the split monitors will also [track the highest lastAppliedOpTime seen](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_utils.cpp#L329) for each recipient node so that we can later choose which node to elect as the recipient primary.

    Once all nodes have correctly reported the recipient set name the donor will [send a replSetStepUp command](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L850) to the node with the highest `lastAppliedOpTime`, guaranteeing that the election will succeed. After sending this command the donor will [wait for a majority write](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L856) on the recipient by sending an `appendOplogNote` command with a majority write concern to the new recipient primary. We need to ensure that the new primary’s first oplog entry is majority committed otherwise it’s possible for a node with an older `lastAppliedOpTime` to become elected, and cause the chosen recipient primary to rollback before its `lastAppliedOpTime`.

5. **Committing the split**
   Finally, the donor commits the split decision by performing an [update to its state document to the kCommitted state](https://github.com/mongodb/mongo/blob/646eed48d0da896588759030f2ec546ac6fbbd48/src/mongo/db/serverless/shard_split_donor_service.cpp#L869-L870). Users requests which were blocked will now be rejected with a `TenantMigrationCommitted` error, indicating that the sender should update its routing tables, and retry the request against the recipient.

### Error Handling

`commitShardSplit` will return [TenantMigrationCommitted](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/shard_split_commands.cpp#L171-L173), [CommandFailed](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/shard_split_commands.cpp#L166-L169), <code>[ConflictingServerlessOperation](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/serverless_operation_lock_registry.cpp#L52-L54)</code>, or any retryable errors encountered during the operation’s execution. On retryable error, callers are expected to retry the operation against the new donor primary. A ConflictingServerlessOperation <em>may </em>be retried, however the caller should do extra work to ensure the conflicting operation has completed before retrying.

### Access Blocking

[Access blockers](#access-blocking-1) are [installed](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L155-L161) on all nodes as soon as a split operation performs its first state transition to kAbortingIndexBuilds. They are initially configured to allow all reads and writes. When the donor primary transitions to the kBlocking state (entering the critical section) it first instructs its access blockers to begin [blocking writes](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_service.cpp#L918), ensuring that no writes to tenant data can commit with a timestamp after the `blockTimestamp`. We begin to block reads once the kBlocking state document [update is committed](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L201). Writes begin blocking on secondaries when the kBlocking state change is [committed on the secondary](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L195), this ensures that an access blocker is already installed and blocking writes if there is donor primary failover.

Access blockers are removed when the state document backing a shard split operation is [deleted](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L437). Since garbage collection of split operation state documents is [not immediate](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_service.cpp#L1178-L1182), access blockers will continue to block reads and writes to tenant data for some time after the operation has completed its critical section. If the split operation is aborted, then access blockers will be removed as soon as the state document [records a decision and is marked garbage-collectable ](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L297-L304)(the `expireAt` field is set). Otherwise, access blockers will be removed when [the state document is deleted](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L435-L438). Access blockers are removed from recipient nodes [after installing the recipient config](https://github.com/mongodb/mongo/blob/e476ee17e9258f540d97a51baf471f5496488e33/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L878-L887), they are no longer donors. a

Access blockers may be removed in a few other scenarios:

-   [When the shard split namespace is dropped](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L456)
-   [When it fails to insert the initial state document](https://github.com/mongodb/mongo/blob/87b60722e3c5ddaf7bc73d1ba08b31b437ef4f48/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L168-L169)

Access blockers are recovered

-   On startup after the [local config is loaded](https://github.com/mongodb/mongo/blob/65154f6a1356de6ca09e04975a0acdfb1a0351ef/src/mongo/db/repl/replication_coordinator_impl.cpp#L537)
-   After initial sync has completed in [InitialSyncer::\_teardown](https://github.com/mongodb/mongo/blob/65154f6a1356de6ca09e04975a0acdfb1a0351ef/src/mongo/db/repl/initial_syncer.cpp#L580)
-   On rollback during the [RollbackImpl::\_runPhaseFromAbortToReconstructPreparedTxns](https://github.com/mongodb/mongo/blob/65154f6a1356de6ca09e04975a0acdfb1a0351ef/src/mongo/db/repl/rollback_impl.cpp#L655)

### Cleanup

Once a shard slit operation has completed it will return either [CommandFailed](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/shard_split_commands.cpp#L166-L169) (if the operation was aborted for any reason), or [TenantMigrationCommitted](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/shard_split_commands.cpp#L171-L173) (if the operation succeeded). At this point it is the caller’s responsibility to take any necessary post-operation actions (such as updating routing tables), before calling `forgetShardSplit` on the donor primary. Calling this command will cause the donor primary to mark the operation garbage-collectable, by [setting the expireAt field in the operation state document](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/shard_split_donor_service.cpp#L1140-L1141) to a configurable timeout called `repl::shardSplitGarbageCollectionDelayMS` with a [default value of 15 minutes](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/repl/repl_server_parameters.idl#L688-L696). The operation will wait for the delay and then [delete the state document](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/shard_split_donor_service.cpp#L1186), which in turn removes access blockers installed for the operation. It is now the responsibility of the caller to remove orphaned data on the donor and recipient.

### Serverless server parameter

The [replication.serverless](https://github.com/mongodb/mongo/blob/e75a51a7dcbe842e07a24343438706d865de96dc/src/mongo/db/mongod_options_replication.idl#L77) server parameter allows starting a mongod without providing a replica set name. It cannot be used at the same time as [replication.replSet](https://github.com/mongodb/mongo/blob/e75a51a7dcbe842e07a24343438706d865de96dc/src/mongo/db/mongod_options_replication.idl#L64) or [replication.replSetName](https://github.com/mongodb/mongo/blob/e75a51a7dcbe842e07a24343438706d865de96dc/src/mongo/db/mongod_options_replication.idl#L70). When `replication.serverless` is used, the replica set name is learned through [replSetInitiate](https://www.mongodb.com/docs/manual/reference/command/replSetInitiate/) or [through an hearbeat](https://github.com/mongodb/mongo/blob/e75a51a7dcbe842e07a24343438706d865de96dc/src/mongo/db/repl/replication_coordinator_impl.cpp#L5848) from another mongod. Mongod can only learn its replica set name once.

Using `replication.serverless` also enables a node to apply a recipient config to join a new recipient set as part of a split.

### Glossary

**recipient config**
The config for the recipient replica set.

**split config**
A config based on the original config which excludes the recipient nodes, and includes a recipient config in a subdocument.

**blockTimestamp**
Timestamp after which reads and writes are blocked on the donor replica set for all tenants involved until completion of the split.

## Shard Merge

A shard split is one of the serverless scaling primitives, allowing for scale in by migrating all tenant data from an underutilized replica set to another existing replica set. The initial replica set will be decomissioned by the cloud control plane after completion of the operation.

The following diagram illustrates the lifetime of a shard split operation:
![shard_merge_diagram](../../../../docs/images/shard_merge_diagram.png)

### Protocol

1. **Start the merge operation**
   The donor primary receives the `donorStartMigration` command to begin the operation. The [TenantMigrationDonorOpObserver](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/tenant_migration_donor_op_observer.cpp#L82) creates a donor access blocker for each tenant and a global donor access blocker.

    All active index builds for collections belonging to tenants which will be migrated are [aborted](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/tenant_migration_donor_service.cpp#L949-L968) at the start of the merge operation. All index builds for tenants being migrated will be blocked for the duration of the operation.

    The donor then reserves an oplog slot, called the `startMigrationDonorTimestamp`. It then [durably records](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/tenant_migration_donor_service.cpp#L982) a state document update to the `kDataSync` state at the `startMigrationDonorTimestamp` and sends the `recipientSyncData` command to the recipient primary with the `startMigrationDonorTimestamp` and waits for a response.

2. **Recipient copies donor data**
   The recipient primary receives the `recipientSyncData` command and [durably persists](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_service.cpp#L2428) a state document used to track migration progress. The [ShardMergeRecipientOpObserver](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_op_observer.cpp#L163-L167) creates a recipient access blocker for each tenant. The primary then opens a backup cursor on the donor, records the checkpoint timestamp, and then inserts the list of wired tiger files that need to be cloned into the [donated files collection](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_service.cpp#L1034-L1046) The backup cursor is kept alive (by periodic `getMore`'s) until all recipient nodes have copied donor data. Wiredtiger will not modify file data on the donor while the cursor is open.

    Additionally, the recipient primary will ensure that it's majority commit timestamp is greater than the backup cursor timestamp from the donor. We [advance](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/repl/shard_merge_recipient_service.cpp#L1787-L1789) the cluster time to `donorBackupCursorCheckpointTimestamp` and then write a majority committed noop.

    A `ShardMergeRecipientOpObserver` on each recipient node will [watch for inserts](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_op_observer.cpp#L198) into the donated files collection and then [clone and import](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/tenant_file_importer_service.cpp#L299-L303) all file data via the `TenantFileImporterService`. When the data is consistent and all files have been imported, the recipient replies `OK` to the `recipientSyncData` command and kills the backup cursor.

3. **Donor enters blocking state**
   Upon receiving a `recipientSyncData` response, the donor reserves an oplog slot and updates the state document to the `kBlocking` state and sets the `blockTimestamp` to prevent writes. The donor then sends a second `recipientSyncData` command to the recipient with the `returnAfterReachingDonorTimestamp` set to the `blockTimestamp` and waits for a reply.

4. **Recipient oplog catchup**
   After the cloned data is consistent, the recipient primary enters the oplog catchup phase. Here, the primary fetches and [applies](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_service.cpp#L2230) any donor oplog entries that were written between the backup cursor checkpoint timestamp and the `blockTimestamp`. When all entries have been majority replicated and we have [ensured](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_service.cpp#L599-L602) that the recipient's logical clock has advanced to at least `returnAfterReachingDonorTimestamp`, the recipient replies `OK` to the second `recipientSyncData` command.

5. **Committing the merge**
   After receiving a successful response to the `recipientSyncData` command, the Donor updates its state document to `kCommitted` and sets the `commitOrAbortOpTime`. After the commit, the Donor will respond to `donorStartMigration` with `OK`. At this point, all traffic should be re-routed to the Recipient. Finally, cloud will send `donorForgetMigration` to the Donor (which will in turn send `recipientForgetMigration` to the Recipient) to mark the migration as garbage collectable.

## Access Blocking

During the critical section of a serverless operation the server will queue user requests for data involved in the operation, waiting to produce a response until after the critical section has completed. This process is called “blocking”, and the server provides this functionality by maintaining a [map of namespace to tenant access blocker](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/repl/tenant_migration_access_blocker_registry.h#L242-L243). This registry is consulted when deciding to block:

-   **commands** in the ServiceEntryPoint ([InvokeCommand::run](https://github.com/mongodb/mongo/blob/bc57b7313bce890cf1a7d6cdf20f1ec25949698f/src/mongo/db/service_entry_point_common.cpp#L886-L888), or [CheckoutSessionAndInvokeCommand::run](https://github.com/mongodb/mongo/blob/bc57b7313bce890cf1a7d6cdf20f1ec25949698f/src/mongo/db/service_entry_point_common.cpp#L886-L888))
-   **linearizable reads** in the [RunCommandImpl::\_epilogue](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/service_entry_point_common.cpp#L1249)
-   **writes** in [OpObserverImpl::onBatchedWriteCommit](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/op_observer/op_observer_impl.cpp#L1882-L1883), [OpObserverImpl::onUnpreparedTransactionCommit](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/op_observer/op_observer_impl.cpp#L1770-L1771), and the [\_logOpsInner oplog helper](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/repl/oplog.cpp#L429-L430)
-   **index builds** in [ReplIndexBuildState::tryAbort](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/repl_index_build_state.cpp#L495), IndexBuildsCoordinatorMongod::\_startIndexBuild ([here](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/index_builds_coordinator_mongod.cpp#L282), [here](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/index_builds_coordinator_mongod.cpp#L356-L357))

## Mutual Exclusion

Of the three types of serverless operation (tenant migration, shard merge, and shard split), no new operation may start if there are any active operations of another serverless operation type. The serverless operation lock allows multiple Tenant Migrations to run simultaneously, but it does not allow running operations of a different type at the same time.

This so-called “serverless operation lock” is acquired the first time a state document is inserted for a particular operation ([shard split](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L150-L151), [tenant migration donor](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/repl/tenant_migration_donor_op_observer.cpp#L58-L60), [tenant migration recipient](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/repl/tenant_migration_recipient_op_observer.cpp#L127-L129), [shard merge recipient](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_op_observer.cpp#L152-L154)). Once the lock is acquired, any attempt to insert a state document of a different operation type will [result in a ConflictingServerlessOperation](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/serverless_operation_lock_registry.cpp#L52-L54). The lock is released when an operation durably records its decision, and marks its state document as garbage collectable ([shard split](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/serverless/shard_split_donor_op_observer.cpp#L261-L263), [tenant migration donor](https://github.com/mongodb/mongo/blob/1c4fafd4ae5c082f36a8af1442aa48174962b1b4/src/mongo/db/repl/tenant_migration_donor_op_observer.cpp#L169-L171), [tenant migration recipient](https://github.com/mongodb/mongo/blob/a723af8863c5fae1eee7b0a891066e923468e974/src/mongo/db/repl/tenant_migration_recipient_op_observer.cpp#L152-L154), [shard merge recipient](https://github.com/mongodb/mongo/blob/f05053d2cb65b84eaed4db94c25e9fe4be82d78c/src/mongo/db/repl/shard_merge_recipient_op_observer.cpp#L280-L282)). Serverless operation locks continue to be held even after a stepdown for the same reason access blockers do, if an election occurs later we ensure the lock is already held to prevent conflicting operations on the newly elected primary.

## Change Streams

Change Stream data for a Serverless cluster is stored in a handful of tenantId-prefixed collections:

-   change collection: `<tenantId>_config.system.change_collection`
-   pre-images: `<tenantId>_config.system.preimages`
-   cluster parameters: `<tenantId>_config.system.cluster_parameters`

A Shard Split operation will copy these collections from donor to recipient via Initial Sync. Upon completion, these collections will be cleaned up on the donor (by the cloud control plane) along with all other tenant-specific databases.

A Shard Merge operation will copy these collections from donor to recipient via backup cursor. For writes that take place during the oplog catchup phase, some additional handling is required in order to ensure correctness of the data written to the tenant's change collection and pre-image collection.

We extract the 'o2' entry from a given noop oplog entry written during this phase (which will contain the original entry on the donor timeline) and write it to the tenant's change collection (see [here](https://github.com/mongodb/mongo/blob/26a441e07f3885dc8b3d9ef9b564eb4f5143bded/src/mongo/db/change_stream_change_collection_manager.cpp#L133-L135) for implementation details). Change collection entries written on the recipient during oplog catchup must be written on the donor timeline so that a change stream can be resumed on the recipient after the Shard Merge.

For pre-image support, two oplog entry fields (`donorOpTime` and `donorApplyOpsIndex`, see [here](https://github.com/mongodb/mongo/blob/26a441e07f3885dc8b3d9ef9b564eb4f5143bded/src/mongo/db/repl/oplog_entry.idl#L168-L180)) were added in order to ensure that pre-image entries written on the recipient will be identical to those on the donor. These fields are conditionally set on oplog entries written during the oplog catchup phase of a Shard Merge and used to determine which timestamp and applyOps index to use when writing pre-images. See [here](https://github.com/mongodb/mongo/blob/07b38e091b48acd305469d525b81aebf3aeadbf1/src/mongo/db/repl/oplog.cpp#L1237-L1268) for details.
