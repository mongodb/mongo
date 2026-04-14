/**
 * This test ensures that the SBE plans join opt produces don't crash the server after unexpectedly
 * encountering an array in a join field after a yield during query execution. This "surprise" array
 * field is facilitated by updating an existing index to multi-key while the query is yielding during
 * query execution.
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 *   featureFlagPathArrayness
 * ]
 */
import {assertAllJoinsUseMethod} from "jstests/libs/query/join_utils.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
assert.commandWorked(conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

function resetCollections(foreignColl, localColl) {
    assert(foreignColl.drop());
    assert(localColl.drop());
    assert.commandWorked(foreignColl.insert(Array.from({length: 1000}, (_, i) => ({_id: i, a: i, b: i}))));
    assert.commandWorked(localColl.insert(Array.from({length: 1000}, (_, i) => ({_id: i, a: i, b: i}))));
    // Add index for multikeyness info for path arrayness.
    assert.commandWorked(foreignColl.createIndex({dummy: 1, a: 1, b: 1}));
    assert.commandWorked(localColl.createIndex({dummy: 1, a: 1, b: 1}));
}

function runPipelineThroughAllJoinMethods({pipeline, localColl, foreignColl}) {
    const joinMethods = ["HJ", "INLJ", "NLJ"];

    for (const joinAlgo of joinMethods) {
        resetCollections(foreignColl, localColl);
        assert.commandWorked(db.adminCommand({setParameter: 1, internalJoinMethod: joinAlgo}));
        /**
         * We need to ensure that JOO query plans can handle encountering a surprise multikey document on the outer and inner sides on a join.
         * At the time this test was written, there was no easy way to enforce which collection is on which side of the join.
         *
         * As an alternative, we can get this same coverage by yielding during execution and inserting an array value into each collection's
         * join field. That way the query plans encounters a surprise multikey field on both sides of the join.
         */
        assert.commandWorked(localColl.createIndex({a: 1}));
        assert.commandWorked(foreignColl.createIndex({b: 1}));
        const explain = localColl.explain().aggregate(pipeline);
        assertAllJoinsUseMethod(explain, joinAlgo);

        let cursor = localColl.aggregate(pipeline, {cursor: {batchSize: 1}});

        cursor.next(); /* Exhaust the client's local cursor's buffer because batchsize is just 1. */

        /**
         * Since the entire result set is greater than the batch size, the server is yielding when the first next()
         * call returns. While the server is yielding, make the foreign and local indexes multikey by inserting an array in
         * the indexed field.
         */
        assert.commandWorked(foreignColl.insert({a: 900, b: [2, 300]}));
        assert.commandWorked(localColl.insert({a: [5, 6, 7], b: 4}));

        while (cursor.hasNext()) {
            /**
             * Exhaust the server's cursor to ensure the JOO query plan encounters the multikey document during execution
             * without crashing the server. This loop is the essential assessment of the test because the join-opt
             * infrastructure currently *assumes* that the join fields are always non-multikey without ever actually
             * *verifying* it; hence why we want to make sure we don't crash!
             */
            cursor.next();
        }
    }
}

const pipeline = [
    {
        $lookup: {
            from: db.collTwo.getName(),
            localField: "a",
            foreignField: "b",
            as: "out",
        },
    },
    {$unwind: "$out"},
];

runPipelineThroughAllJoinMethods({
    pipeline,
    localColl: db.collOne,
    foreignColl: db.collTwo,
});

MongoRunner.stopMongod(conn);
