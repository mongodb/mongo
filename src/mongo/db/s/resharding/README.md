# Resharding internals

Resharding is the way for users to redistribute their data across the cluster. It is critical to
help maintain the data distribution and achieve high query performance in a sharded cluster.

# ReshardCollection Command

The resharding operation starts from a `reshardCollection` command from the user. The client sends a
`reshardCollection` command to mongos and mongos sends an internal `_shardsvrReshardCollection`
command [to the primary of admin database](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/s/commands/cluster_reshard_collection_cmd.cpp#L121).
The primary will [create a `ReshardCollectionCoordinatorDocument` and `ReshardCollectionCoordinator`](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/shardsvr_reshard_collection_command.cpp#L106). The `RechardCollectionCoordinator` then issues
[`_configsvrReshardCollection` command to the config shard](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/reshard_collection_coordinator.cpp#L177) to ask the config server to start the resharding process.

The config server will do some validations for the incoming reshardCollection request and then
create a `ReshardingCoordinatorDocument` and `ReshardingCoordinator`, which will drive the state
machine of resharding coordinator.

# Resharding State Machine

The whole resharding process is operated by three state machines, the `ReshardingCoordinatorService`,
the `ReshardingDonorService` and the `ReshardingRecipientService`. Each of the three state machines
refers to its corresponding state document called [ReshardingCoordinatorDocument](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/coordinator_document.idl),
[ReshardingDonorDocument](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/donor_document.idl)
and [ReshardingRecipientDocument](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/recipient_document.idl).
The `ReshardingDonorService` and `ReshardingRecipientService` will be started by the monitoring
thread once [it sees the resharding fields on the collection](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/shard_filtering_metadata_refresh.cpp#L464-L468). They also use the state documents to notify the other state machines during certain
states to make sure the whole process is moving forward.

## ReshardingCoordinatorService

As the name telling, this is the service to coordinate the resharding process. We'll break down
its responsibility by its states.

### Initializing

The ReshardingCoordinatorService will [insert the coordinator doc](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_coordinator_service.cpp#L931) to `config.reshardingOperations` and [add `recipientFields`](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_coordinator_service.cpp#L935) to the collection metadata. After that, it
[calculates the participant shards](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_coordinator_service.cpp#L2195)
and move the `kPreparingToDonate` state.

### Preparing to Donate

The coordinator will [add donorFields](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_coordinator_service.cpp#L2219)
to the collection metadata so the `ReshardingDonorService` will be started. Then the coordinator
will wait until [all donor shards have reported their `minFetchTimestamp`](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_coordinator_service.cpp#L2263) and move to `kCloning` state. The coordinator will pick
the highest `minFetchTimestamp` as the `cloneTimestamp` for all the recipient shards to perform
collection cloning on a snapshot at this timestamp.

Note: the `minFetchTimestamp` is a timestamp that a donor shard guarantees that after this
timestamp, all oplog entries on this donor shard contain recipient shard information.

### Cloning

The coordinator notifies all recipients to refresh so they can start cloning. The coordinator then
simply [wait until all recipient shards finish cloning](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_coordinator_service.cpp#L2292) and then move to `kApplying` state.

### Applying

The coordinator will [wait until the `_canEnterCritial` future to be fulfilled](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_coordinator_service.cpp#L2344) then move to `kBlockingWrites`.

### Blocking Writes

The coordinator will wait until all recipients are in `kStrictConsistency` state then it will move
to `kCommit`.

### Committing

In this state, the coordinator is to keep the donors and recipients in sync on switching the
original collection and the temporary resharding collection. Once they all successfully commit the
changes of this resharding, the reshardCollection is considered as success.

### Quiesced

The `kQuiesced` state is introduced to avoid a wrong attempt to start a new resharding operation.
It helps in the case of retrying a reshardCollection. In this state, we keep the coordinator doc
for a certain period of time so if the client issues the same `reshardCollection` again, we know
it's a duplicate and won't run resharding one more time.

## ReshardingDonorService

### Preparing to Donate

The donor will [do a no-op write and use the OpTime as the `minFetchTimestamp`](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_donor_service.cpp#L655), so this can make sure that all future oplog entries on this
donor shard contain the recipient shard information. After reporting the `minFetchTimestamp` to
the coordinator, the donor is ready to be cloned.

### Donating Initial Data

The donor will wait the coordinator to coordinate the cloning process and let the recipients clone
data. It will move to `kDonatingOplogEntries` state once the cloning is completed.

### Donating Oplog Entries

The donor will wait the coordinator to coordinate the applying oplog stage where recipient will
fetch and apply oplog entries that are written after the `minFetchTimestamp`.

### Preparing to Block Writes

The donor will [write a no-op oplog](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_donor_service.cpp#L776)
to block writes on the source collection.

### Blocking Writes

The donor shard will block writes until it gets a decision from the coordinator whether this
resharding can be committed or not.

## ReshardingRecipientService

### Awaiting Fetch Timestamp

The recipient service waits until the coordinator collects `minFetchTimeStamp` from all donor shards,
then transition to `kCreatingCollection` state.

### Creating Collection

The recipient service will create a temporary collection where the recipient will write all the
data that is supposed to be in this shard into this temp collection. After resharding is committed,
it will drop the original collection and change this temp collection to replace the old collection.

There is an optimization here that the recipient [won't create any index](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_recipient_service.cpp#L702) except the `_id` index. This helps to reduce the write
amplification during cloning. All indexes will be recreated during the `kBuildingIndex` state.

### Cloning

This is the most time consuming part of resharding where the recipient shards clone all needed data
to this shard based on the new shardKey range. The [Resharding Collection Cloning](#resharding-collection-cloning)
section will go into more details of the cloning process.

### Building Index

The recipient will find out all the indexes that exist on the old collection and [create them](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_recipient_service.cpp#L904) on the new collection including the shardKey index
if necessary. The index building process is driven by the `IndexBuildsCoordinator`.

### Applying

The recipient wait all oplog entries that are written after the `cloneTimestamp` to be applied on the
recipient shard. This is done together with the cloning part in `ReshardingDataReplication`, which
we will cover in the [Resharding Collection Cloning](#resharding-collection-cloning) section.

### Strict Consistency

The recipient service will enter strict consistency and wait the coordinator to collect the status
of all donors and shards to decide whether this resharding operation can be committed.

# Resharding Collection Cloning

The collection cloning includes two parts, clone the collection at a certain timestamp and apply
oplog entries after that timestamp.

## ReshardingCollectionCloner

The collection cloner is running as part of `ReshardingRecipientService`, which reads the needed
collection data from the donor shard, then write the data into the temp resharding collection. This
is achieved by a natural order scan on the donor. The `ReshardingCollectionCloner` [crafts a query](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_collection_cloner.cpp#L564) to do the natural order scan with a [resume token](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_collection_cloner.cpp#L589), which helps it retry on any transient error.

The cloning is done in parallel on the recipient by `ReshardingClonerFetcher`, where we can have
[multiple reader threads](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_collection_cloner.cpp#L503)
to do reads against different donor shards and [multiple writer threads](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_collection_cloner.cpp#L502) to write the fetched data on the recipient. The writer thread
also needs to [maintain the resumeToken](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_collection_cloner.cpp#L664)
for that shard, so one important thing here is that all data from the same donor shard should be
inserted sequentially, which is implemented in a way that all data from one donor shard will only be
processed by one writer thread.

## ReshardingOplogFetcher and ReshardingOplogApplier

The `ReshardingOplogFetcher` [fetches oplog entries from `minFetchTimestamp`](https://github.com/mongodb/mongo/blob/c8778bfa3b21e9f6c6ac125ca48b816dc1994bf0/src/mongo/db/s/resharding/resharding_data_replication.cpp#L158-L161) on the corresponding donor shard and the
`ReshardingOplogApplier` will apply those oplog entries to the recipient shard.
