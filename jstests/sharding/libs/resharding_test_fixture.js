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

        this._dbName = undefined;
        this._collName = undefined;
        this._ns = undefined;
        this._sourceCollectionUUIDString = undefined;

        this._tempCollName = undefined;
        this._tempNs = undefined;

        this._st = undefined;
        this._reshardingThread = undefined;
        this._pauseCoordinatorInSteadyStateFailpoint = undefined;
        this._newShardKey = undefined;
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

    get temporaryReshardingCollectionName() {
        return this._tempCollName;
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
        const sourceDB = sourceCollection.getDB();

        this._dbName = sourceDB.getName();
        this._collName = sourceCollection.getName();

        CreateShardedCollectionUtil.shardCollectionWithChunks(
            sourceCollection, shardKeyPattern, chunks);

        const sourceCollectionUUID =
            getUUIDFromListCollections(sourceDB, sourceCollection.getName());
        this._sourceCollectionUUIDString = extractUUIDFromObject(sourceCollectionUUID);

        this._tempCollName = `system.resharding.${this._sourceCollectionUUIDString}`;
        this._tempNs = `${this._dbName}.${this._tempCollName}`;

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

        this._newShardKey = newShardKeyPattern;

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

    _checkConsistencyPostReshardingComplete() {
        ///
        // Check that resharding content on the configsvr is cleaned up.
        ///
        assert.eq(0, this._st.config.reshardingOperations.find({nss: this._ns}).itcount());

        assert.eq(
            0,
            this._st.config.collections.find({reshardingFields: {$exists: true}, _id: this._ns})
                .itcount());

        assert.eq(0, this._st.config.collections.find({_id: this._tempNs}).itcount());

        ///
        // Check that resharding content local to each participant is cleaned up.
        ///
        this._donorShards().forEach((donor) => {
            assert.eq(0,
                      donor.getDB("config")
                          .localReshardingOperations.donor.find({nss: this._ns})
                          .itcount());
        });

        this._recipientShards().forEach((recipient) => {
            assert(!recipient.getCollection(this._tempNs).exists());
            assert.eq(0,
                      recipient.getDB("config")
                          .localReshardingOperations.recipient.find({nss: this._ns})
                          .itcount());
        });

        ///
        // Check that the collection is updated from the resharding operation.
        ///
        const finalReshardedCollectionUUID =
            getUUIDFromListCollections(this._st.s.getDB(this._dbName), this._collName);
        assert.neq(this._sourceCollectionUUIDString,
                   extractUUIDFromObject(finalReshardedCollectionUUID));

        const actualShardKey = this._st.config.collections.findOne({_id: this._ns}).key;
        assert.eq(this._newShardKey, actualShardKey);
    }

    teardown() {
        this._pauseCoordinatorInSteadyStateFailpoint.wait();
        const pauseCoordinatorBeforeCommitFailpoint =
            configureFailPoint(this._pauseCoordinatorInSteadyStateFailpoint.conn,
                               "reshardingPauseCoordinatorBeforeCommit");

        this._pauseCoordinatorInSteadyStateFailpoint.off();
        pauseCoordinatorBeforeCommitFailpoint.wait();

        this._checkConsistency();

        pauseCoordinatorBeforeCommitFailpoint.off();
        this._reshardingThread.join();

        this._checkConsistencyPostReshardingComplete();

        this._st.stop();
    }
};
