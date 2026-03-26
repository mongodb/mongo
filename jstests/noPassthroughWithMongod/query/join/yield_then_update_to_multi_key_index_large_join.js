/**
 * This test ensures that the SBE plans join opt produces don't crash the server after unexpectedly
 * encountering an array in a join field after a yield during query execution. This test in particular
 * creates a pipeline of 8 $lookup-$unwind pairs that fan out from the base coll via a distinct join
 * field.
 *
 * While yielding during execution, an array value is inserted into each collection's indexed field.
 * The test then continues execution to ensure the unexpected array value doesn't put the server
 * in an unrecoverable state.
 * @tags: [
 *   requires_fcv_83,
 *   requires_sbe
 * ]
 */

import {joinOptUsed} from "jstests/libs/query/join_utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

function* charRange(startChar, endChar) {
    for (let code = startChar.charCodeAt(0); code <= endChar.charCodeAt(0); code++) {
        yield String.fromCharCode(code);
    }
}

function resetCollections(db, localColl) {
    localColl.drop();
    const docs = [
        {a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1, h: 1},
        {a: 2, b: 2, c: 2, d: 2, e: 2, f: 2, g: 2, h: 2},
        {a: 3, b: 3, c: 3, d: 3, e: 3, f: 3, g: 3, h: 3},
        {a: 4, b: 4, c: 4, d: 4, e: 4, f: 4, g: 4, h: 4},
        {a: 5, b: 5, c: 5, d: 5, e: 5, f: 5, g: 5, h: 5},
        {a: 6, b: 6, c: 6, d: 6, e: 6, f: 6, g: 6, h: 6},
        {a: 7, b: 7, c: 7, d: 7, e: 7, f: 7, g: 7, h: 7},
        {a: 8, b: 8, c: 8, d: 8, e: 8, f: 8, g: 8, h: 8},
    ];
    assert.commandWorked(localColl.insertMany(docs));
    /* Iterate over A through H of the alphabet */
    for (const joinField of charRange("a", "h")) {
        const from = `coll${joinField.toUpperCase()}`;
        const coll = db[from];
        coll.drop();
        assert.commandWorked(coll.insertMany(docs));

        const key = {[joinField]: 1};
        assert.commandWorked(coll.createIndex(key));
        assert.commandWorked(localColl.createIndex(key));
    }
}
/**
 * Create a pipeline of 8 $lookup-$unwind pairs that fan out from the base coll
 * via a distinct join field (first 'a', then 'b', then 'c'...).
 * This includes, for each LU pair, creating a new "foreign" collection and
 * an index on the given join field on the foreign collection and the local collection.
 */
function buildOutCollectionsIndexesAndPipeline(db, localColl) {
    resetCollections(db, localColl);
    let pipeline = [];
    /* Iterate over A through H of the alphabet */
    for (const joinField of charRange("a", "h")) {
        const from = `coll${joinField.toUpperCase()}`;
        const localField = joinField;
        const foreignField = joinField;
        pipeline.push({"$lookup": {from, localField, foreignField, as: from}});
        pipeline.push({"$unwind": {path: `$${from}`}});
    }
    return pipeline;
}

function runPipeline({pipeline, localColl}) {
    /* Seeding with the same, arbitrary int will guarantee determinism. */
    Random.setRandomSeed(20230328);
    /* Running the randomizer through a loop 50 times will generate good join order, shape, and method coverage.*/
    for (let i = 0; i < 50; i++) {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalRandomJoinOrderSeed: Random.randInt(10000)}));
        let explain = localColl.explain().aggregate(pipeline);
        assert(joinOptUsed(explain));
        let cursor = localColl.aggregate(pipeline, {cursor: {batchSize: 1}});

        cursor.next(); /* Exhaust the client's local cursor's buffer because batchsize is just 1. */
        /**
         * Since the entire result set is greater than the batch size, the server is yielding when the first next()
         * call returns. While the server is yielding, make every index across all collections multikey by inserting an array in
         * the collection's indexed field.
         */
        for (const joinField of charRange("a", "h")) {
            const from = `coll${joinField.toUpperCase()}`;
            const coll = db[from];

            const arr = Array.from({length: 8}, () => Random.randInt());
            const doc = {[joinField]: [arr]};
            assert.commandWorked(coll.insert(doc));
            assert.commandWorked(localColl.insert(doc));
        }
        while (cursor.hasNext()) {
            /**
             * Exhaust the server's cursor to ensure the JOO query plan encounters the multikey document during execution
             * without crashing the server. This loop is the essential assessment of the test because the join-opt
             * infrastructure currently *assumes* that the join fields are always non-multikey without ever actually
             * *verifying* it; hence why we want to make sure we don't crash!
             */
            cursor.next();
        }
        resetCollections(db, localColl);
    }
}

const localColl = db.baseColl;

const pipeline = buildOutCollectionsIndexesAndPipeline(db, localColl);
runPipeline({pipeline, localColl});
MongoRunner.stopMongod(conn);
