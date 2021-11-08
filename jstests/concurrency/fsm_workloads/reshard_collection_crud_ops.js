'use strict';

/**
 * Runs reshardCollection and CRUD operations concurrently.
 *
 * @tags: [requires_sharding]
 */

var $config = (function() {
    const shardKeys = [
        {a: 1},
        {b: 1},
    ];

    const data = {
        shardKey: shardKeys[0],
        currentShardKeyIndex: 0,
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

    function executeReshardCommand(db, collName, newShardKey) {
        const coll = db.getCollection(collName);
        print(`Started Resharding Collection ${coll.getFullName()}. New Shard Key ${
            tojson(newShardKey)}`);
        if (TestData.runningWithShardStepdowns) {
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({reshardCollection: coll.getFullName(), key: newShardKey}),
                [ErrorCodes.SnapshotUnavailable]);
        } else {
            assert.commandWorked(
                db.adminCommand({reshardCollection: coll.getFullName(), key: newShardKey}));
        }
        print(`Finished Resharding Collection ${coll.getFullName()}. New Shard Key ${
            tojson(newShardKey)}`);
    }

    const states = {
        insert: function insert(db, collName) {
            const coll = db.getCollection(collName);
            print(`Inserting documents for collection ${coll.getFullName()}.`);
            const totalDocumentsToInsert = 10;
            assert.commandWorked(coll.insert(createDocuments(totalDocumentsToInsert)));
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

                executeReshardCommand(db, collName, shardKey);
                // If resharding fails with SnapshopUnavailable, then this will be incorrect. But
                // its fine since reshardCollection will succeed if the new shard key matches the
                // existing one.
                this.currentShardKeyIndex = shardKeyIndex;
                this.reshardingCount += 1;
            }
        }
    };

    const transitions = {
        reshardCollection: {insert: 1},
        insert: {insert: .5, reshardCollection: .5}
    };

    function setup(db, collName, _cluster) {
        const coll = db.getCollection(collName);
        assert.commandWorked(coll.insert(createDocuments(kTotalWorkingDocuments)));
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
