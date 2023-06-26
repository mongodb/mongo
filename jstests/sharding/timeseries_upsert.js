/**
 * Verifies upserts on sharded timeseries collection.
 *
 * @tags: [
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   # Upsert on sharded timeseries collection is only supported in FCV 7.1+
 *   requires_fcv_71,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");
load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

setUpShardedCluster();

const collName = 'sharded_timeseries_upsert';
const dateTime = new ISODate();

function generateDocsForTestCase(collConfig) {
    let documents = [];
    for (let i = 0; i < 4; i++) {
        documents.push({
            _id: i,
            [metaFieldName]: collConfig.metaGenerator(i),
            [timeFieldName]: dateTime,
            f: i
        });
    }
    return documents;
}

const metaShardKey = {
    metaGenerator: (id => id),
    shardKey: {[metaFieldName]: 1},
    splitPoint: {meta: 2},
};
const metaSubFieldShardKey = {
    metaGenerator: (index => ({a: index})),
    shardKey: {[metaFieldName + '.a']: 1},
    splitPoint: {'meta.a': 2},
};

function runTest({collConfig, updateOp, upsertedDoc, errorCode, inTxn = false}) {
    // Prepares a sharded timeseries collection.
    const documents = generateDocsForTestCase(collConfig);
    const coll = prepareShardedCollection({
        collName,
        initialDocList: documents,
        shardKey: collConfig.shardKey,
        splitPoint: collConfig.splitPoint
    });

    // Performs updates.
    const updateCommand = {update: coll.getName(), updates: [updateOp]};
    if (errorCode) {
        assert.commandFailedWithCode(testDB.runCommand(updateCommand), errorCode);
        return;
    }
    const res = (() => {
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

    if (upsertedDoc) {
        assert.eq(res.upserted.length, 1, tojson(res));
        upsertedDoc["_id"] = res.upserted[0]._id;
        documents.push(upsertedDoc);
    }

    const resultDocs = coll.find().toArray();
    assert.eq(resultDocs.length, documents.length);

    assert.sameMembers(
        resultDocs, documents, "Collection contents did not match expected after upsert");
}

//
// Tests for multi updates.
//

(function testMultiUpdateNoShardKey() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {q: {f: 1000}, u: {$set: {[timeFieldName]: dateTime}}, multi: true, upsert: true},
        errorCode: ErrorCodes.ShardKeyNotFound,
    });
})();

(function testMultiUpdateNoEqualityOperator() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: {$gt: 0}, f: 1000},
            u: {$set: {[timeFieldName]: dateTime}},
            multi: true,
            upsert: true
        },
        errorCode: ErrorCodes.ShardKeyNotFound,
    });
})();

(function testMultiUpdateUnsetShardKey() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: [{$set: {[timeFieldName]: dateTime, f: 15}}, {$unset: metaFieldName}],
            multi: true,
            upsert: true
        },
        upsertedDoc: {[timeFieldName]: dateTime, f: 15},
    });
})();

(function testMultiUpdateUpsertShardKeyFromQuery() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: {$set: {[timeFieldName]: dateTime, f: 15}},
            multi: true,
            upsert: true
        },
        upsertedDoc: {[metaFieldName]: -1, [timeFieldName]: dateTime, f: 15},
    });
})();

(function testMultiUpdateUpsertShardKeyFromUpdate() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: {$set: {[metaFieldName]: -10, [timeFieldName]: dateTime, f: 15}},
            multi: true,
            upsert: true
        },
        upsertedDoc: {[metaFieldName]: -10, [timeFieldName]: dateTime, f: 15},
    });
})();

(function testMultiUpdateUpsertShardKeyArrayNotAllowed() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: {$set: {[metaFieldName]: [1, 2, 3], [timeFieldName]: dateTime, f: 15}},
            multi: true,
            upsert: true
        },
        errorCode: ErrorCodes.NotSingleValueField,
    });
})();

//
// Tests for a nested shard key.
//

(function testMultiUpdateNestedShardKeyNotFound() {
    runTest({
        collConfig: metaSubFieldShardKey,
        updateOp: {
            q: {[metaFieldName]: 1},
            u: {$set: {[timeFieldName]: dateTime}},
            multi: true,
            upsert: true
        },
        errorCode: ErrorCodes.ShardKeyNotFound,
    });
})();

(function testMultiUpdateNestedShardKeyNoEqualityOperator() {
    runTest({
        collConfig: metaSubFieldShardKey,
        updateOp: {
            q: {[metaFieldName + '.a']: {$gt: 0}, f: 1000},
            u: {$set: {[timeFieldName]: dateTime}},
            multi: true,
            upsert: true
        },
        errorCode: ErrorCodes.ShardKeyNotFound,
    });
})();

(function testMultiUpdateUnsetNestedShardKey() {
    runTest({
        collConfig: metaSubFieldShardKey,
        updateOp: {
            q: {[metaFieldName + '.a']: -1},
            u: [
                {$set: {[metaFieldName + '.b']: 10, [timeFieldName]: dateTime, f: 15}},
                {$unset: metaFieldName + '.a'}
            ],
            multi: true,
            upsert: true
        },
        upsertedDoc: {[metaFieldName]: {b: 10}, [timeFieldName]: dateTime, f: 15},
    });
})();

(function testMultiUpdateUpsertNestedShardKeyFromQuery() {
    runTest({
        collConfig: metaSubFieldShardKey,
        updateOp: {
            q: {[metaFieldName + '.a']: -1},
            u: {$set: {[timeFieldName]: dateTime, f: 15}},
            multi: true,
            upsert: true
        },
        upsertedDoc: {[metaFieldName]: {a: -1}, [timeFieldName]: dateTime, f: 15},
    });
})();

(function testMultiUpdateUpsertNestedShardKeyFromUpdate() {
    runTest({
        collConfig: metaSubFieldShardKey,
        updateOp: {
            q: {[metaFieldName + '.a']: -1},
            u: {$set: {[metaFieldName + '.a']: -10, [timeFieldName]: dateTime, f: 15}},
            multi: true,
            upsert: true
        },
        upsertedDoc: {[metaFieldName]: {a: -10}, [timeFieldName]: dateTime, f: 15},
    });
})();

(function testMultiUpdateUpsertNestedShardKeyArrayNotAllowed() {
    runTest({
        collConfig: metaSubFieldShardKey,
        updateOp: {
            q: {[metaFieldName + '.a']: -1},
            u: {$set: {[metaFieldName + '.a']: [1, 2, 3], [timeFieldName]: dateTime, f: 15}},
            multi: true,
            upsert: true
        },
        errorCode: ErrorCodes.NotSingleValueField,
    });
})();

//
// Tests for singleton updates.
//

(function testSingleUpdateNoShardKey() {
    runTest({
        collConfig: metaShardKey,
        updateOp:
            {q: {f: 1000}, u: {$set: {[timeFieldName]: dateTime}}, multi: false, upsert: true},
        upsertedDoc: {f: 1000, [timeFieldName]: dateTime},
    });
})();

(function testSingleUpdateNoEqualityOperator() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: {$gt: 0}, f: 1000},
            u: {$set: {[timeFieldName]: dateTime}},
            multi: false,
            upsert: true
        },
        upsertedDoc: {f: 1000, [timeFieldName]: dateTime},
    });
})();

(function testSingleUpdateWithShardKey() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: {$set: {[metaFieldName]: -10, [timeFieldName]: dateTime, f: 15}},
            multi: false,
            upsert: true
        },
        upsertedDoc: {[metaFieldName]: -10, [timeFieldName]: dateTime, f: 15},
    });
})();

(function testSingleUpdateReplacementDocWouldChangeOwningShard() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: {[metaFieldName]: 10, [timeFieldName]: dateTime, f: 15},
            multi: false,
            upsert: true
        },
        upsertedDoc: {[metaFieldName]: 10, [timeFieldName]: dateTime, f: 15},
        inTxn: true,
    });
})();

(function testSingleUpdateReplacementDocWithNoShardKey() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: {[timeFieldName]: dateTime, f: 15},
            multi: false,
            upsert: true
        },
        upsertedDoc: {[timeFieldName]: dateTime, f: 15}
    });
})();

(function testSingleUpdateUpsertSuppliedWithNoShardKey() {
    runTest({
        collConfig: metaShardKey,
        updateOp: {
            q: {[metaFieldName]: -1},
            u: [{$set: {unused: true}}],
            multi: false,
            upsert: true,
            upsertSupplied: true,
            c: {new: {[timeFieldName]: dateTime, f: 15}},
        },
        upsertedDoc: {[timeFieldName]: dateTime, f: 15}
    });
})();

st.stop();
})();
