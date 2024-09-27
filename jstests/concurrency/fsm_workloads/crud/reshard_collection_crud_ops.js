/**
 * Runs reshardCollection and CRUD operations concurrently.
 *
 * @tags: [requires_sharding]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export const $config = (function() {
    const shardKeys = [
        {a: 1},
        {b: 1},
    ];

    const data = {
        shardKey: shardKeys[0],
        currentShardKeyIndex: 0,
        reshardingCount: 0,
    };

    const iterations = 25;
    const kTotalWorkingDocuments = 1000;
    const kMaxReshardingExecutions = TestData.runningWithShardStepdowns ? 4 : iterations;

    /**
     * @summary Takes in a number of documents to create, creates each document. With two properties
     * being equal to the index and one counter property.
     * @param {number} numDocs
     * @returns {Array{Object}} an array of documents to be inserted into the collection.
     */
    function createDocuments(numDocs) {
        const documents = Array.from({length: numDocs}).map((_, i) => ({a: i, b: i, c: 0}));
        return documents;
    }

    function executeReshardCommand(db, collName, newShardKey, forceRedistribution) {
        const coll = db.getCollection(collName);
        print(`Started Resharding Collection ${coll.getFullName()}. New Shard Key ${
            tojson(newShardKey)}, Same key resharding ${forceRedistribution}`);
        let reshardCollectionCmd = {reshardCollection: coll.getFullName(), key: newShardKey};
        if (forceRedistribution) {
            reshardCollectionCmd.forceRedistribution = forceRedistribution;
        }
        if (TestData.runningWithShardStepdowns) {
            assert.commandWorkedOrFailedWithCode(db.adminCommand(reshardCollectionCmd),
                                                 [ErrorCodes.SnapshotUnavailable]);
        } else {
            assert.commandWorked(db.adminCommand(reshardCollectionCmd));
        }
        print(`Finished Resharding Collection ${coll.getFullName()}. New Shard Key ${
            tojson(newShardKey)}`);
    }

    const states = {
        insert: function insert(db, collName) {
            const coll = db.getCollection(collName);
            print(`Inserting documents for collection ${coll.getFullName()}.`);
            const totalDocumentsToInsert = 5;
            assert.soon(() => {
                try {
                    coll.insert(createDocuments(totalDocumentsToInsert));
                    return true;
                } catch (err) {
                    if (err instanceof BulkWriteError && err.hasWriteErrors()) {
                        for (let writeErr of err.getWriteErrors()) {
                            if (writeErr.code == 11000) {
                                // 11000 is a duplicate key error. If the insert generates the same
                                // _id object as another concurrent insert, retry the command.
                                return false;
                            }
                        }
                    }
                    throw err;
                }
            });
            print(`Finished Inserting documents.`);
        },
        reshardCollection: function reshardCollection(db, collName) {
            //'reshardingMinimumOperationDurationMillis' is set to 30 seconds when there are
            // stepdowns. So in order to limit the overall time for the test, we limit the number of
            // resharding operations to kMaxReshardingExecutions.
            const shouldContinueResharding = this.reshardingCount <= kMaxReshardingExecutions;
            if (this.tid === 0 && shouldContinueResharding) {
                const currentShardKeyIndex = this.currentShardKeyIndex;
                const newIndex = (currentShardKeyIndex + 1) % shardKeys.length;
                const shardKey = shardKeys[newIndex];

                executeReshardCommand(db, collName, shardKey, false /*forceRedistribution*/);
                // If resharding fails with SnapshopUnavailable, then this will be incorrect. But
                // its fine since reshardCollection will succeed if the new shard key matches the
                // existing one.
                this.currentShardKeyIndex = newIndex;
                this.reshardingCount += 1;
            }
        },
        reshardCollectionSameKey: function reshardCollectionSameKey(db, collName) {
            const shouldContinueResharding = this.reshardingCount <= kMaxReshardingExecutions;
            if (this.tid === 0 && shouldContinueResharding) {
                const currentShardKeyIndex = this.currentShardKeyIndex;
                const newIndex = this._allowSameKeyResharding
                    ? currentShardKeyIndex
                    : (currentShardKeyIndex + 1) % shardKeys.length;
                const shardKey = shardKeys[newIndex];

                executeReshardCommand(db, collName, shardKey, this._allowSameKeyResharding);
                // If resharding fails with SnapshopUnavailable, then this will be incorrect. But
                // its fine since reshardCollection will succeed if the new shard key matches the
                // existing one.
                this.currentShardKeyIndex = newIndex;
                this.reshardingCount += 1;
            }
        }
    };

    const transitions = {
        reshardCollection: {insert: 1},
        reshardCollectionSameKey: {insert: 1},
        insert: {insert: .5, reshardCollection: .25, reshardCollectionSameKey: .25}
    };

    function setup(db, collName, _cluster) {
        const coll = db.getCollection(collName);
        assert.commandWorked(coll.insert(createDocuments(kTotalWorkingDocuments)));
        this._allowSameKeyResharding =
            FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'ReshardingImprovements');
    }

    return {
        threadCount: 20,
        iterations: iterations,
        startState: 'reshardCollection',
        states: states,
        transitions: transitions,
        setup: setup,
        data: data
    };
})();
