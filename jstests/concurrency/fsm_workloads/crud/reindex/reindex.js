/**
 * reindex.js
 *
 * Bulk inserts 1000 documents and builds indexes. Then alternates between reindexing and querying
 * against the collection. Operates on a separate collection for each thread.
 *
 * The reIndex command is only available on standalone mode.
 *
 * @tags: [SERVER-40561, requires_getmore, requires_standalone,
 * incompatible_with_concurrency_simultaneous]
 */
import {assertWorkedHandleTxnErrors} from "jstests/concurrency/fsm_workload_helpers/assert_handle_fail_in_transaction.js";

export const $config = (function () {
    let data = {
        nIndexes: 4 + 1, // 4 created and 1 for _id.
        nDocumentsToInsert: 1000,
        maxInteger: 100, // Used for document values. Must be a factor of nDocumentsToInsert.
        prefix: "reindex", // Use filename for prefix because filename is assumed unique.
    };

    let states = (function () {
        function insertDocuments(db, collName) {
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = 0; i < this.nDocumentsToInsert; ++i) {
                bulk.insert({
                    text:
                        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do" +
                        " eiusmod tempor incididunt ut labore et dolore magna aliqua.",
                    geo: {type: "Point", coordinates: [(i % 50) - 25, (i % 50) - 25]},
                    integer: i % this.maxInteger,
                });
            }
            let res = bulk.execute();
            assert.commandWorked(res);
            assert.eq(this.nDocumentsToInsert, res.nInserted);
        }

        function createIndexes(db, collName) {
            // The number of indexes created here is also stored in data.nIndexes.
            const coll = db[this.threadCollName];
            assertWorkedHandleTxnErrors(coll.createIndex({text: "text"}), ErrorCodes.IndexBuildAlreadyInProgress);
            assertWorkedHandleTxnErrors(coll.createIndex({geo: "2dsphere"}), ErrorCodes.IndexBuildAlreadyInProgress);
            assertWorkedHandleTxnErrors(coll.createIndex({integer: 1}), ErrorCodes.IndexBuildAlreadyInProgress);
            assertWorkedHandleTxnErrors(coll.createIndex({"$**": 1}), ErrorCodes.IndexBuildAlreadyInProgress);
        }

        function init(db, collName) {
            this.threadCollName = this.prefix + "_" + this.tid;
            insertDocuments.call(this, db, this.threadCollName);
        }

        function query(db, collName) {
            let coll = db[this.threadCollName];
            let nInsertedDocuments = this.nDocumentsToInsert;
            let count = coll.find({integer: Random.randInt(this.maxInteger)}).itcount();
            assert.eq(
                nInsertedDocuments / this.maxInteger,
                count,
                "number of " + "documents returned by integer query should match the number " + "inserted",
            );

            let coords = [
                [
                    [-26, -26],
                    [-26, 26],
                    [26, 26],
                    [26, -26],
                    [-26, -26],
                ],
            ];
            let geoQuery = {geo: {$geoWithin: {$geometry: {type: "Polygon", coordinates: coords}}}};

            // We can only perform a geo query when we own the collection and are sure a geo index
            // is present. The same is true of text queries.
            count = coll.find(geoQuery).itcount();
            assert.eq(
                count,
                nInsertedDocuments,
                "number of documents returned by" + " geospatial query should match number inserted",
            );

            count = coll.find({$text: {$search: "ipsum"}}).itcount();
            assert.eq(
                count,
                nInsertedDocuments,
                "number of documents returned by" + " text query should match number inserted",
            );

            let indexCount = db[this.threadCollName].getIndexes().length;
            assert.eq(indexCount, this.nIndexes);
        }

        function reIndex(db, collName) {
            let res = db[this.threadCollName].reIndex();
            assert.commandWorked(res);
        }

        return {init: init, createIndexes: createIndexes, reIndex: reIndex, query: query};
    })();

    let transitions = {
        init: {createIndexes: 1},
        createIndexes: {reIndex: 0.5, query: 0.5},
        reIndex: {reIndex: 0.5, query: 0.5},
        query: {reIndex: 0.5, query: 0.5},
    };

    return {threadCount: 15, iterations: 10, states: states, transitions: transitions, data: data};
})();
