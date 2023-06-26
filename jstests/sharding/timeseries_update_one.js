/**
 * Tests 'updateOne' command on sharded collections.
 *
 * @tags: [
 *   # To avoid multiversion tests
 *   requires_fcv_71,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");

setUpShardedCluster();
const testDB = getTestDB();

const runTest = function({
    initialDocList,
    query,
    update,
    nModified,
    resultDocList,
    includeMeta = true,
    inTxn = false,
}) {
    const collName = getCallerName();
    jsTestLog(`Running ${collName}(${tojson(arguments[0])})`);

    // Creates and shards a timeseries collection.
    const coll = prepareShardedCollection({collName: collName, initialDocList, includeMeta});

    const updateCommand = {update: collName, updates: [{q: query, u: update, multi: false}]};
    const result = (() => {
        if (!inTxn) {
            return assert.commandWorked(testDB.runCommand(updateCommand));
        }

        // TODO SERVER-78364 Run this as a retryable write instead of inside a transaction.
        const session = coll.getDB().getMongo().startSession();
        const sessionDb = session.getDatabase(coll.getDB().getName());
        session.startTransaction();
        const res = assert.commandWorked(sessionDb.runCommand(updateCommand));
        session.commitTransaction();

        return res;
    })();
    assert.eq(nModified, result.n, tojson(result));

    if (resultDocList) {
        assert.sameMembers(resultDocList,
                           coll.find().toArray(),
                           "Collection contents did not match expected after update");
    } else {
        assert.eq(coll.countDocuments({}),
                  initialDocList.length,
                  "Collection count did not match expected after update: " +
                      tojson(coll.find().toArray()));
    }
};

(function testTargetSingleShardByMeta() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {[metaFieldName]: "A"},
        update: {$set: {f: 110}},
        nModified: 1,
        resultDocList: [
            {_id: 2, [metaFieldName]: "A", [timeFieldName]: generateTimeValue(2), f: 110},
            doc4_b_f103
        ],
    });
})();

(function testTargetSingleShardByMetaNoMatches() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {[metaFieldName]: "C"},
        update: {$set: {f: 110}},
        nModified: 0,
        resultDocList: [doc2_a_f101, doc4_b_f103],
    });
})();

(function testTargetSingleShardByMetaWithAdditionalMetricFilter() {
    runTest({
        initialDocList: [doc2_a_f101, doc3_a_f102, doc4_b_f103],
        query: {[metaFieldName]: "A", f: 102},
        update: {$set: {f: 110}},
        nModified: 1,
        resultDocList: [
            doc2_a_f101,
            {_id: 3, [metaFieldName]: "A", [timeFieldName]: generateTimeValue(3), f: 110},
            doc4_b_f103
        ],
    });
})();

(function testTargetSingleShardByTime() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        includeMeta: false,
        query: {[timeFieldName]: generateTimeValue(2)},
        update: {$set: {f: 110}},
        nModified: 1,
        resultDocList: [
            {_id: 2, [metaFieldName]: "A", [timeFieldName]: generateTimeValue(2), f: 110},
            doc4_b_f103
        ],
    });
})();

(function testTargetSingleShardUnsetShardKey() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {[metaFieldName]: "A"},
        update: {$unset: {[metaFieldName]: 1}},
        nModified: 1,
        resultDocList: [{_id: 2, [timeFieldName]: generateTimeValue(2), f: 101}, doc4_b_f103],
        inTxn: true,
    });
})();

(function testTargetSingleShardUnsetShardKeyByReplacement() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {[metaFieldName]: "A"},
        update: {[timeFieldName]: generateTimeValue(2), f: 110},
        replacement: true,
        nModified: 1,
        resultDocList: [{_id: 2, [timeFieldName]: generateTimeValue(2), f: 110}, doc4_b_f103],
        inTxn: true,
    });
})();

(function testTargetSingleShardUpdateShardKeyByReplacement() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {[metaFieldName]: "B"},
        update: {[metaFieldName]: "C", [timeFieldName]: generateTimeValue(4), f: 110},
        replacement: true,
        nModified: 1,
        resultDocList: [
            doc2_a_f101,
            {_id: 4, [metaFieldName]: "C", [timeFieldName]: generateTimeValue(4), f: 110}
        ],
        inTxn: true,
    });
})();

(function testTargetSingleShardUpdateShardKeyByReplacementChangeShard() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {[metaFieldName]: "B"},
        update: {[metaFieldName]: "A", [timeFieldName]: generateTimeValue(4), f: 110},
        replacement: true,
        nModified: 1,
        resultDocList: [
            doc2_a_f101,
            {_id: 4, [metaFieldName]: "A", [timeFieldName]: generateTimeValue(4), f: 110}
        ],
        inTxn: true,
    });
})();

(function testTwoPhaseUpdate() {
    runTest({
        initialDocList: [doc2_a_f101, doc3_a_f102, doc4_b_f103, doc6_c_f105],
        query: {f: {$gt: 100}},
        update: {$set: {f: 110}},
        nModified: 1,
    });
})();

(function testTwoPhaseUpdateNoMatches() {
    runTest({
        initialDocList: [doc2_a_f101, doc3_a_f102, doc4_b_f103, doc6_c_f105],
        query: {f: {$gt: 1000}},
        update: {$set: {f: 110}},
        nModified: 0,
        resultDocList: [doc2_a_f101, doc3_a_f102, doc4_b_f103, doc6_c_f105],
    });
})();

(function testTwoPhaseUpdateById() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {_id: 4},
        update: {$set: {f: 110}},
        nModified: 1,
        resultDocList: [
            doc2_a_f101,
            {_id: 4, [metaFieldName]: "B", [timeFieldName]: generateTimeValue(4), f: 110},
        ],
    });
})();

(function testTwoPhaseUpdateByTimeField() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103],
        query: {[timeFieldName]: generateTimeValue(4)},
        update: {$set: {f: 110}},
        nModified: 1,
        resultDocList: [
            doc2_a_f101,
            {_id: 4, [metaFieldName]: "B", [timeFieldName]: generateTimeValue(4), f: 110},
        ],
    });
})();

(function testTwoPhaseUpdateEmptyPredicate() {
    runTest({
        initialDocList: [doc2_a_f101, doc4_b_f103, doc6_c_f105],
        query: {},
        update: {$set: {f: 110}},
        nModified: 1,
    });
})();

st.stop();
})();
