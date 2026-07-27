/**
 * Verifies `$_internalSearchIdLookup` returns identical results across batch sizes.
 *
 * With featureFlagSearchOptimizedIdLookup on, idLookup resolves `_id`s a window at a time via the
 * Express/SBE executor, capped by 'internalSearchIdLookupMaxBatchSize'. The batch size is a perf
 * knob and must not change the result set: a size of 1 forces a window per `_id` (exercising the
 * multi-window refill path, including windows where every event is dropped), while larger sizes
 * span fewer windows. The collection holds more documents than the mid-range batch size so that
 * size also crosses multiple windows.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = "internal_search_id_lookup_batching";

// Doubling each source `_id` (1..numDocs) yields look-up targets 2,4,...,2*numDocs; only the even
// targets that exist in the collection (2..numDocs) are returned, the higher ones are dropped. With
// more documents than the mid-range batch size, both the found docs and the interleaved drops span
// several windows even at maxBatchSize=10.
const numDocs = 24;
const expectedFoundDocs = [];
for (let id = 2; id <= numDocs; id += 2) {
    expectedFoundDocs.push({_id: id, x: "doc" + id});
}

const idLookupPipeline = [
    {$match: {}},
    {$addFields: {idToLookFor: {$toInt: "$_id"}}},
    {$project: {"_id": {$multiply: ["$idToLookFor", 2]}}},
    {$_internalSearchIdLookup: {}},
    {$sort: {"_id": 1}},
];

// $_internalSearchIdLookup is internal-only: rejected in user requests and accepted only from an
// internal client, which must specify an explicit (empty) writeConcern on commands that accept one.
function idLookupAgg(internalDB, collectionName, pipeline) {
    const res = assert.commandWorked(
        internalDB.runCommand({
            aggregate: collectionName,
            pipeline: pipeline,
            cursor: {},
            readConcern: {},
            writeConcern: {},
        }),
    );
    return new DBCommandCursor(internalDB, res);
}

for (const batchSize of [1, 10, 100]) {
    describe(`$_internalSearchIdLookup (maxBatchSize=${batchSize})`, function () {
        before(function () {
            this.conn = MongoRunner.runMongod();
            this.db = this.conn.getDB("test");
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    internalSearchIdLookupMaxBatchSize: batchSize,
                }),
            );

            const coll = this.db[collName];
            coll.drop();
            for (let id = 1; id <= numDocs; id++) {
                assert.writeOK(coll.insert({_id: id, x: "doc" + id}));
            }

            // Internal client to exercise the internal-only stage, leaving the main connection as a
            // normal client so setup writes and shutdown validation hooks are unaffected.
            this.internalConn = new Mongo(this.db.getMongo().host);
            assert.commandWorked(
                this.internalConn.getDB("admin").runCommand({
                    hello: 1,
                    internalClient: {
                        minWireVersion: NumberInt(0),
                        maxWireVersion: NumberInt(7),
                    },
                }),
            );
            this.internalDB = this.internalConn.getDB("test");
        });

        after(function () {
            this.internalConn.close();
            MongoRunner.stopMongod(this.conn);
        });

        it("returns the same found documents regardless of batch size", function () {
            assert.eq(
                expectedFoundDocs,
                idLookupAgg(this.internalDB, collName, idLookupPipeline).toArray(),
            );
        });

        it("drops missing `_id`s across window boundaries", function () {
            assert.eq(
                expectedFoundDocs.length,
                idLookupAgg(this.internalDB, collName, idLookupPipeline).itcount(),
            );
        });

        it("honors a downstream $limit", function () {
            const limited = idLookupAgg(this.internalDB, collName, [
                ...idLookupPipeline,
                {$limit: 2},
            ]).toArray();
            assert.eq(expectedFoundDocs.slice(0, 2), limited);
        });
    });
}
