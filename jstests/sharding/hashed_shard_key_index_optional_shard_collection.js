/*
 * Test shardCollection commands against non-existent collections or existent collections (empty or
 * non-empty) without a shard key index.
 * 1. If the shardCollection command is run with "implicitlyCreateIndex" true:
 *    - If the collection is non-existent or empty, then the command succeeds and the shard key
 *      index is created.
 *    - Otherwise, the command fails with InvalidOptions.
 * 2. If the shardCollection command is run with "implicitlyCreateIndex" false:
 *    - If the shard key is hashed and featureFlagHashedShardKeyIndexOptionalUponShardingCollection
 *      is enabled, then the command succeeds and the shard key index doesn't get created.
 *    - Otherwise, the command fails with 6373200.
 *
 * For each resulting sharded collection, check that:
 * - find commands that filter by shard key equality uses IXSCAN if the collection has a shard key
 *   index (because the shardCollection command was run with "implicitlyCreateIndex" true) or a
 *   non-shard key compatible index.
 * - moveChunk commands against the given hashed sharded collection fails with IndexNotFound if
 *   the shardCollection command was run with "implicitlyCreateIndex" false.
 *
 * Consider the shard key {x: "hashed"}, the shard key indexes for this shard key include
 * {x: "hashed"}, {x: "hashed", y: 1} and the non-shard key but compatible indexes for this shard
 * key include {x: 1}, {x: 1, y: 1} and {x: 1, y: "hashed"}.
 *
 * @tags: [
 *   requires_fcv_81,
 *   featureFlagHashedShardKeyIndexOptionalUponShardingCollection
 * ]
 */

import {
    getWinningPlanFromExplain,
} from "jstests/libs/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

const dbName = "testDb";
let shardNames = [];

function makeDocument(shardKey, indexKey, val) {
    let doc = {};
    for (let fieldName in shardKey) {
        AnalyzeShardKeyUtil.setDottedField(doc, fieldName, val);
    }
    if (indexKey) {
        for (let fieldName in indexKey) {
            AnalyzeShardKeyUtil.setDottedField(doc, fieldName, val);
        }
    }
    return doc;
}

function checkIfIndexExists(st, dbName, collName, indexKey) {
    const indexes = st.s.getDB(dbName).getCollection(collName).getIndexes();
    jsTest.log("Checking indexes " + tojson({dbName, collName, indexes}));
    return indexes.some(index => bsonWoCompare(index.key, indexKey) == 0);
}

/*
 * Checks that find commands that filter by shard key equality against the given sharded
 * collection uses IXSCAN if the collection has a shard key index (because the shardCollection
 * command was run with "implicitlyCreateIndex" true) or a non-shard key compatible index.
 */
function testFindCommand(st, dbName, collName, doc, testCase) {
    const coll = st.s.getDB(dbName).getCollection(collName);

    const shardKeyValue =
        AnalyzeShardKeyUtil.extractShardKeyValueFromDocument(doc, testCase.shardKey);
    const explain = coll.find(shardKeyValue).explain();
    const winningPlan = getWinningPlanFromExplain(explain);

    const isIdHashedShardKey = bsonWoCompare(testCase.shardKey, {_id: "hashed"}) == 0;
    if (isIdHashedShardKey) {
        const isClusteredColl = AnalyzeShardKeyUtil.isClusterCollection(st.s, dbName, collName);
        if (isClusteredColl) {
            assert.eq(winningPlan.stage, "EXPRESS_CLUSTERED_IXSCAN", winningPlan);
            assert(!winningPlan.keyPattern);
        } else {
            assert.eq(winningPlan.stage, "EXPRESS_IXSCAN", winningPlan);
            assert.eq(winningPlan.keyPattern, "{ _id: 1 }", winningPlan);
        }
    } else if (testCase.implicitlyCreateIndex || testCase.containsCompatibleIndex) {
        assert.eq(winningPlan.stage, "FETCH", winningPlan);
        assert.eq(winningPlan.inputStage.stage, "IXSCAN", winningPlan);
        if (testCase.implicitlyCreateIndex) {
            assert(bsonWoCompare(winningPlan.inputStage.keyPattern, testCase.shardKey) == 0 ||
                       bsonWoCompare(winningPlan.inputStage.keyPattern, testCase.indexKey) == 0,
                   winningPlan);
        } else {
            assert(bsonWoCompare(winningPlan.inputStage.keyPattern, testCase.indexKey) == 0,
                   winningPlan);
        }
    }
}

/*
 * Checks that moveChunk commands against the given sharded collection fails with IndexNotFound if
 * the shardCollection command was run with "implicitlyCreateIndex" false.
 */
function testMoveChunkCommand(st, dbName, collName, doc, testCase) {
    const ns = dbName + "." + collName;
    const shardKeyValue =
        AnalyzeShardKeyUtil.extractShardKeyValueFromDocument(doc, testCase.shardKey);
    let failed = false;
    for (let shardName of shardNames) {
        const res = st.s.adminCommand({moveChunk: ns, find: shardKeyValue, to: shardName});
        if (testCase.implicitlyCreateIndex) {
            assert.commandWorked(res);
        } else {
            assert.commandWorkedOrFailedWithCode(res, ErrorCodes.IndexNotFound);
            if (res.code == ErrorCodes.IndexNotFound) {
                failed = true;
            }
        }
    }
    if (!testCase.implicitlyCreateIndex) {
        assert(failed);
    }
}

function testNonExistentCollection(st, testCase) {
    jsTest.log("Test sharding a non-existent collection " + tojson(testCase));
    const collName1 = "testColl1";
    const ns1 = dbName + "." + collName1;
    const coll = st.s.getCollection(ns1);

    const res = st.s.adminCommand({
        shardCollection: ns1,
        key: testCase.shardKey,
        implicitlyCreateIndex: testCase.implicitlyCreateIndex
    });
    if (testCase.expectErrorCode) {
        assert.commandFailedWithCode(res, testCase.expectErrorCode);
        const expectedIndexExists = false;
        assert.eq(checkIfIndexExists(st, dbName, collName1, testCase.shardKey),
                  expectedIndexExists,
                  {testCase});
    } else {
        assert.commandWorked(res);
        const expectedIndexExists = testCase.implicitlyCreateIndex;
        assert.eq(checkIfIndexExists(st, dbName, collName1, testCase.shardKey),
                  expectedIndexExists,
                  {testCase});

        const doc = makeDocument(testCase.shardKey, testCase.indexKey, 1);
        assert.commandWorked(coll.insert(doc));
        testFindCommand(st, dbName, collName1, doc, testCase);
        testMoveChunkCommand(st, dbName, collName1, doc, testCase, expectedIndexExists);
    }

    assert(coll.drop());
}

function testExistentCollection(st, testCase) {
    jsTest.log("Test sharding an existent collection " + tojson(testCase));
    const collName2 = "testColl2";
    const ns2 = dbName + "." + collName2;
    const db = st.s.getDB(dbName);
    const coll = db.getCollection(collName2);
    let doc;

    if (testCase.indexKey) {
        assert.commandWorked(coll.createIndex(testCase.indexKey));
    } else {
        assert.commandWorked(db.createCollection(collName2));
    }
    if (!testCase.isEmptyCollection) {
        doc = makeDocument(testCase.shardKey, testCase.indexKey, 1);
        assert.commandWorked(coll.insert(doc));
    }

    const res = st.s.adminCommand({
        shardCollection: ns2,
        key: testCase.shardKey,
        implicitlyCreateIndex: testCase.implicitlyCreateIndex
    });
    if (testCase.expectErrorCode) {
        assert.commandFailedWithCode(res, testCase.expectErrorCode);
        const expectedIndexExists = false;
        assert.eq(checkIfIndexExists(st, dbName, collName2, testCase.shardKey),
                  expectedIndexExists,
                  {testCase});
    } else {
        assert.commandWorked(res);
        const expectedIndexExists = testCase.implicitlyCreateIndex;
        assert.eq(checkIfIndexExists(st, dbName, collName2, testCase.shardKey),
                  expectedIndexExists,
                  {testCase});

        if (testCase.isEmptyCollection) {
            doc = makeDocument(testCase.shardKey, testCase.indexKey, 2);
            assert.commandWorked(coll.insert(doc));
        }
        testFindCommand(st, dbName, collName2, doc, testCase);
        testMoveChunkCommand(st, dbName, collName2, doc, testCase, expectedIndexExists);
    }

    assert(coll.drop());
}

// Test cases of non-shard key indexes. shardCollection commands with 'implicitlyCreateIndex' false
// should only succeed when the shard key is hashed and
// featureFlagHashedShardKeyIndexOptionalUponShardingCollection is true.
const shardKeyTestCases = [
    {
        shardKey: {_id: "hashed"},
        indexKeyTestCases: [
            {key: {a: "hashed"}, isCompatible: false},
            {key: {a: 1}, isCompatible: false},
        ]
    },
    {
        shardKey: {a: 1},
        indexKeyTestCases: [
            {key: {a: "hashed"}, isCompatible: true},
        ]
    },
    {
        shardKey: {a: "hashed"},
        indexKeyTestCases: [
            {key: {a: 1}, isCompatible: true},
            {key: {a: 1, b: 1}, isCompatible: true},
            {key: {a: 1, b: "hashed"}, isCompatible: true},
            {key: {b: 1, a: "hashed"}, isCompatible: false},
            {key: {b: 1, a: 1}, isCompatible: false},
        ]
    },
    {
        shardKey: {a: 1, b: 1},
        indexKeyTestCases: [
            {key: {a: "hashed", b: 1}, isCompatible: true},
            {key: {a: 1, b: "hashed"}, isCompatible: true},
        ]
    },
    {
        shardKey: {a: 1, b: "hashed"},
        indexKeyTestCases: [
            {key: {a: "hashed", b: 1}, isCompatible: true},
            {key: {a: 1, b: 1, c: 1}, isCompatible: true},
            {key: {a: 1}, isCompatible: false},
            {key: {b: "hashed"}, isCompatible: false},
            {key: {b: "hashed", a: 1}, isCompatible: false},
        ]
    },
    {
        shardKey: {"a.x": 1, b: 1},
        indexKeyTestCases: [
            {key: {"a.x": 1, b: "hashed"}, isCompatible: true},
            {key: {"a.x": "hashed", b: 1}, isCompatible: true},
        ]
    },
    {
        shardKey: {"a.x": "hashed", b: 1},
        indexKeyTestCases: [
            {key: {"a.x": 1, b: "hashed"}, isCompatible: true},
            {key: {"a.x": 1, b: 1, c: 1}, isCompatible: true},
            {key: {"a.x": "hashed"}, isCompatible: false},
            {key: {b: 1}, isCompatible: false},
            {key: {b: 1, "a.x": "hashed"}, isCompatible: false},
        ]
    },
];

function runTest(hashedShardKeyIndexOptionalUponShardingCollection,
                 trackUnshardedCollectionsUponCreation) {
    jsTest.log("Testing with " + tojson({
                   hashedShardKeyIndexOptionalUponShardingCollection,
                   trackUnshardedCollectionsUponCreation
               }));
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            setParameter: {
                featureFlagHashedShardKeyIndexOptionalUponShardingCollection:
                    hashedShardKeyIndexOptionalUponShardingCollection,
                featureFlagTrackUnshardedCollectionsUponCreation:
                    trackUnshardedCollectionsUponCreation,
            }
        }
    });
    shardNames = [];
    shardNames.push(st.shard0.shardName);
    shardNames.push(st.shard1.shardName);

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    for (let shardKeyTestCase of shardKeyTestCases) {
        const isHashedShardKey = AnalyzeShardKeyUtil.isHashedKeyPattern(shardKeyTestCase.shardKey);
        jsTest.log("Testing " + tojson({shardKeyTestCase}));

        for (let implicitlyCreateIndex of [true, false]) {
            const isShardKeyIndexOptional =
                hashedShardKeyIndexOptionalUponShardingCollection && isHashedShardKey;
            testNonExistentCollection(st, {
                shardKey: shardKeyTestCase.shardKey,
                implicitlyCreateIndex,
                expectErrorCode: (() => {
                    if (implicitlyCreateIndex) {
                        return null;
                    }
                    return isShardKeyIndexOptional ? null : 6373200;
                })(),
            });

            for (let isEmptyCollection of [true, false]) {
                testExistentCollection(st, {
                    shardKey: shardKeyTestCase.shardKey,
                    isEmptyCollection,
                    implicitlyCreateIndex,
                    expectErrorCode: (() => {
                        if (implicitlyCreateIndex) {
                            return isEmptyCollection ? null : ErrorCodes.InvalidOptions;
                        }
                        return isShardKeyIndexOptional ? null : 6373200;
                    })(),
                });

                for (let indexTestCase of shardKeyTestCase.indexKeyTestCases) {
                    testExistentCollection(st, {
                        shardKey: shardKeyTestCase.shardKey,
                        indexKey: indexTestCase.key,
                        containsCompatibleIndex: indexTestCase.isCompatible,
                        isEmptyCollection,
                        implicitlyCreateIndex,
                        expectErrorCode: (() => {
                            if (implicitlyCreateIndex) {
                                return isEmptyCollection ? null : ErrorCodes.InvalidOptions;
                            }
                            return isShardKeyIndexOptional ? null : 6373200;
                        })(),
                    });
                }
            }
        }
    }

    st.stop();
}

for (let hashedShardKeyIndexOptionalUponShardingCollection of [true, false]) {
    for (let trackUnshardedCollectionsUponCreation of [true, false]) {
        runTest(hashedShardKeyIndexOptionalUponShardingCollection,
                trackUnshardedCollectionsUponCreation);
    }
}
