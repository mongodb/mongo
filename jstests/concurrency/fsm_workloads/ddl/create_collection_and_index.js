/**
 * create_collection_and_index.js
 *
 * Repeatedly creates collections and indexes and verifies the idents are consistent across replica set nodes.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 *   # This test intends to check consistency across a replica set.
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_83,
 * ]
 */

export const $config = (function () {
    var data = {
        emptyCollectionCount: 20,
        dataCollectionCount: 5,
        indexCount: 64,

        getEmptyCollectionName: function () {
            return `empty_coll_${Math.floor(Math.random() * this.emptyCollectionCount)}`;
        },

        getDataCollectionName: function () {
            return `data_coll_${Math.floor(Math.random() * this.dataCollectionCount)}`;
        },

        getIndexField: function () {
            return `field_${Math.floor(Math.random() * this.indexCount)}`;
        },
    };

    var states = (function () {
        function init(db, collName) {
            // Insert some data into 'data_coll_*' collections.
            for (let i = 0; i < this.dataCollectionCount; ++i) {
                assert.commandWorked(db.getCollection(this.getDataCollectionName(i)).insert({a: 1}));
            }
        }

        /**
         * This state can produce 'create' and 'createIndexes' oplog entries.
         */
        function createIndexesOnEmptyCollection(db, collName) {
            const targetCollection = this.getEmptyCollectionName();
            const indexField = this.getIndexField();
            assert.commandWorkedOrFailedWithCode(
                db.getCollection(targetCollection).createIndexes([{[indexField]: 1}]),
                ErrorCodes.CannotCreateIndex,
            );
        }

        /**
         * This state can produce 'create' and 'startIndexBuild' oplog entries.
         */
        function createIndexesOnDataCollection(db, collName) {
            const targetCollection = this.getDataCollectionName();
            const indexField = this.getIndexField();
            assert.commandWorkedOrFailedWithCode(
                db.getCollection(targetCollection).createIndexes([{[indexField]: 1}]),
                ErrorCodes.CannotCreateIndex,
            );
        }

        return {
            init: init,
            createIndexesOnEmptyCollection: createIndexesOnEmptyCollection,
            createIndexesOnDataCollection: createIndexesOnDataCollection,
        };
    })();

    var transitions = {
        init: {createIndexesOnEmptyCollection: 0.8, createIndexesOnDataCollection: 0.2},
        createIndexesOnEmptyCollection: {createIndexesOnEmptyCollection: 0.8, createIndexesOnDataCollection: 0.2},
        createIndexesOnDataCollection: {createIndexesOnEmptyCollection: 0.8, createIndexesOnDataCollection: 0.2},
    };

    function teardown(db, collName, cluster) {
        cluster.awaitReplication();
        // Check each collection or index has the same ident across all nodes in the replica set.
        let identCounts = {};
        let numNodes = 0;
        const dbName = db.getName();
        // Aggregate all idents from all nodes.
        cluster.executeOnMongodNodes((db) => {
            ++numNodes;
            const idents = db
                .getSiblingDB("admin")
                .aggregate([
                    {$listCatalog: {}},
                    // Only check for collections and indexes created by the workload.
                    {$match: {"db": dbName}},
                    {
                        $project: {
                            "name": 1,
                            "idxIdent": 1,
                            "ident": 1,
                        },
                    },
                ])
                .toArray();
            for (let i = 0; i < idents.length; ++i) {
                // Use the combination of [collName, ident] as the key.
                const collKey = [idents[i].name, idents[i].ident];
                identCounts[collKey] = (identCounts[collKey] || 0) + 1;
                for (let idxName in idents[i].idxIdent) {
                    const idxIdent = idents[i].idxIdent[idxName];
                    // Use the combination of [collName, idxName, ident] as the key.
                    const idxKey = [idents[i].name, idxName, idxIdent];
                    identCounts[idxKey] = (identCounts[idxKey] || 0) + 1;
                }
            }
        });

        // Check each ident is present on all nodes.
        for (let ident in identCounts) {
            assert.eq(identCounts[ident], numNodes, `Ident ${ident} doesn't exist on all nodes in the replica set`);
        }
    }

    return {threadCount: 5, iterations: 100, data: data, states: states, transitions: transitions, teardown: teardown};
})();
