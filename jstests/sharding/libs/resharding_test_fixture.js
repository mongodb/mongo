"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/sharding/libs/create_sharded_collection_util.js");

/**
 * Test fixture for resharding a sharded collection once.
 *
 * Example usage:
 *
 *      const reshardingTest = new ReshardingTest();
 *      reshardingTest.setup();
 *      const sourceCollection = reshardingTest.createShardedCollection(...);
 *      // ... Do some operations before resharding starts ...
 *      assert.commandWorked(sourceCollection.insert({_id: 0}));
 *      reshardingTest.startReshardingInBackground(...);
 *      // ... Do some operations during the resharding operation ...
 *      assert.commandWorked(sourceCollection.update({_id: 0}, {$inc: {a: 1}}));
 *      reshardingTest.teardown();
 */
var ReshardingTest = class {
    constructor({
        numDonors: numDonors = 1,
        numRecipients: numRecipients = 1,
        reshardInPlace: reshardInPlace = false,
    } = {}) {
        this._numDonors = numDonors;
        this._numRecipients = numRecipients;
        this._reshardInPlace = reshardInPlace;
        this._numShards = this._reshardInPlace ? Math.max(this._numDonors, this._numRecipients)
                                               : this._numDonors + this._numRecipients;

        this._ns = undefined;
        this._tempNs = undefined;

        this._st = undefined;
        this._reshardingThread = undefined;
        this._pauseCoordinatorInSteadyStateFailpoint = undefined;
    }

    setup() {
        this._st = new ShardingTest({
            mongos: 1,
            config: 1,
            configOptions: {setParameter: {"reshardingTempInterruptBeforeOplogApplication": false}},
            shards: this._numShards,
            rs: {nodes: 2},
            rsOptions: {
                setParameter: {
                    // TODO SERVER-52795: Remove once the donor shards write the final oplog entry
                    // themselves.
                    "failpoint.allowDirectWritesToLiveOplog": tojson({mode: "alwaysOn"}),
                    "failpoint.WTPreserveSnapshotHistoryIndefinitely": tojson({mode: "alwaysOn"}),
                    "reshardingTempInterruptBeforeOplogApplication": false,
                }
            },
            manualAddShard: true,
        });

        for (let i = 0; i < this._numShards; ++i) {
            const isDonor = i < this._numDonors;
            const isRecipient = i >= (this._numShards - this._numRecipients);
            assert(isDonor || isRecipient, {
                i,
                numDonors: this._numDonors,
                numRecipients: this._numRecipients,
                numShards: this._numShards,
            });

            // We add a "-donor" or "-recipient" suffix to the shard's name when it has a singular
            // role during the resharding process.
            let shardName = `shard${i}`;
            if (isDonor && !isRecipient) {
                shardName += `-donor${i}`;
            } else if (isRecipient && !isDonor) {
                shardName += `-recipient${i - this._numDonors}`;
            }

            const shard = this._st[`shard${i}`];
            const res = assert.commandWorked(
                this._st.s.adminCommand({addShard: shard.host, name: shardName}));
            shard.shardName = res.shardAdded;
        }
    }

    _donorShards() {
        return Array.from({length: this._numDonors}, (_, i) => this._st[`shard${i}`]);
    }

    get donorShardNames() {
        return this._donorShards().map(shard => shard.shardName);
    }

    _recipientShards() {
        return Array
            .from({length: this._numRecipients},
                  (_, i) => this._st[`shard${this._numShards - 1 - i}`])
            .reverse();
    }

    get recipientShardNames() {
        return this._recipientShards().map(shard => shard.shardName);
    }

    /**
     * Shards a non-existing collection using the specified shard key and chunk ranges.
     *
     * @param chunks - an array of
     * {min: <shardKeyValue0>, max: <shardKeyValue1>, shard: <shardName>} objects. The chunks must
     * form a partition of the {shardKey: MinKey} --> {shardKey: MaxKey} space.
     */
    createShardedCollection({ns, shardKeyPattern, chunks}) {
        this._ns = ns;

        const sourceCollection = this._st.s.getCollection(ns);
        CreateShardedCollectionUtil.shardCollectionWithChunks(
            sourceCollection, shardKeyPattern, chunks);

        const sourceDB = sourceCollection.getDB();
        const sourceCollectionUUID =
            getUUIDFromListCollections(sourceDB, sourceCollection.getName());
        const sourceCollectionUUIDString = extractUUIDFromObject(sourceCollectionUUID);

        this._tempNs = `${sourceDB.getName()}.system.resharding.${sourceCollectionUUIDString}`;

        return sourceCollection;
    }

    /**
     * Reshards an existing collection using the specified new shard key and new chunk ranges.
     *
     * @param newChunks - an array of
     * {min: <shardKeyValue0>, max: <shardKeyValue1>, shard: <shardName>} objects. The chunks must
     * form a partition of the {shardKey: MinKey} --> {shardKey: MaxKey} space.
     */
    startReshardingInBackground({newShardKeyPattern, newChunks}) {
        newChunks = newChunks.map(
            chunk => ({min: chunk.min, max: chunk.max, recipientShardId: chunk.shard}));

        this._pauseCoordinatorInSteadyStateFailpoint = configureFailPoint(
            this._st.configRS.getPrimary(), "reshardingPauseCoordinatorInSteadyState");

        this._reshardingThread = new Thread(function(host, ns, newShardKeyPattern, newChunks) {
            const conn = new Mongo(host);
            assert.commandWorked(conn.adminCommand({
                reshardCollection: ns,
                key: newShardKeyPattern,
                _presetReshardedChunks: newChunks,
            }));
        }, this._st.s.host, this._ns, newShardKeyPattern, newChunks);

        this._reshardingThread.start();
    }

    _checkConsistency() {
        const nsCursor = this._st.s.getCollection(this._ns).find().sort({_id: 1});
        const tempNsCursor = this._st.s.getCollection(this._tempNs).find().sort({_id: 1});

        const diff = ((diff) => {
            return {
                docsWithDifferentContents: diff.docsWithDifferentContents.map(
                    ({first, second}) => ({original: first, resharded: second})),
                docsExtraAfterResharding: diff.docsMissingOnFirst,
                docsMissingAfterResharding: diff.docsMissingOnSecond,
            };
        })(DataConsistencyChecker.getDiff(nsCursor, tempNsCursor));

        assert.eq(diff, {
            docsWithDifferentContents: [],
            docsExtraAfterResharding: [],
            docsMissingAfterResharding: [],
        });
    }

    _writeFinalOplogEntry() {
        const sourceCollection = this._st.s.getCollection(this._ns);
        const sourceCollectionUUID =
            getUUIDFromListCollections(sourceCollection.getDB(), sourceCollection.getName());

        const tempCollection =
            this._recipientShards()[0].rs.getPrimary().getCollection(this._tempNs);
        const reshardingUUID =
            getUUIDFromListCollections(tempCollection.getDB(), tempCollection.getName());

        for (let donor of this._donorShards()) {
            // We temporarily disable replication's oplog fetching on secondaries before manually
            // inserting the final oplog entry for resharding to ensure that secondaries won't
            // accidentally skip past it.
            stopReplicationOnSecondaries(donor.rs);

            const shardPrimary = donor.rs.getPrimary();
            assert.commandWorked(
                shardPrimary.getDB("local").oplog.rs.insert(this._recipientShards().map(
                    recipient => ({
                        op: "n",
                        ns: this._ns,
                        ui: sourceCollectionUUID,
                        o: {
                            msg: `Writes to ${
                                this._ns} are temporarily blocked for resharding (via mongo shell)`
                        },
                        o2: {type: "reshardFinalOp", reshardingUUID: reshardingUUID},
                        destinedRecipient: recipient.shardName,
                        // fixDocumentForInsert() in the server will replace the Timestamp(0, 0)
                        // value with a cluster time generated from the logical clock.
                        ts: Timestamp(0, 0),
                        t: NumberLong(1),
                        wall: new Date(),
                        v: NumberLong(2),
                    }))));

            // We follow up the direct writes to the oplog with a write to a replicated collection
            // because otherwise the majority-committed snapshot won't advance and the
            // ReshardingOplogFetcher would never see the no-op oplog entries that were inserted.
            assert.commandWorked(shardPrimary.getDB("dummydb").dummycoll.insert({}));

            restartReplicationOnSecondaries(donor.rs);
        }
    }

    teardown() {
        this._pauseCoordinatorInSteadyStateFailpoint.wait();
        const pauseCoordinatorBeforeCommitFailpoint =
            configureFailPoint(this._pauseCoordinatorInSteadyStateFailpoint.conn,
                               "reshardingPauseCoordinatorBeforeCommit");

        // TODO SERVER-52795: Remove once the donor shards write the final oplog entry themselves.
        this._writeFinalOplogEntry();

        this._pauseCoordinatorInSteadyStateFailpoint.off();
        pauseCoordinatorBeforeCommitFailpoint.wait();

        this._checkConsistency();

        pauseCoordinatorBeforeCommitFailpoint.off();
        this._reshardingThread.join();
        // The recipient shards may or may not have renamed the sharded collection by the time the
        // _configsvrReshardCollection command returns.
        //
        // TODO SERVER-52931: Remove once _configsvrReshardCollection waits for the sharded
        // collection to have been renamed on all of the recipient shards.
        TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
        this._st.stop();
    }
};
