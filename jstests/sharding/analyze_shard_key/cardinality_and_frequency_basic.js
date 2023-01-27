/**
 * Tests that the analyzeShardKey command returns correct cardinality and frequency metrics.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

// Define base test cases. For each test case:
// - 'shardKey' is the shard key being analyzed.
// - 'indexKey' is the index that the collection has.
// - 'indexOptions' is the additional options for the index.
const shardKeyPrefixedIndexTestCases = [
    // Test non-compound shard keys with a shard key index.
    {shardKey: {a: 1}, indexKey: {a: 1}, expectMetrics: true},
    {shardKey: {a: "hashed"}, indexKey: {a: "hashed"}, expectMetrics: true},
    {shardKey: {"a.x": 1}, indexKey: {"a.x": 1}, expectMetrics: true},
    {shardKey: {"a.x.y": 1}, indexKey: {"a.x.y": 1}, expectMetrics: true},
    // Test compound shard keys with a shard key index.
    {shardKey: {a: 1, b: 1}, indexKey: {a: 1, b: 1}, expectMetrics: true},
    {shardKey: {"a.x": 1, "b": "hashed"}, indexKey: {"a.x": 1, "b": "hashed"}, expectMetrics: true},
    {shardKey: {"a.x.y": "hashed", b: 1}, indexKey: {"a.x.y": "hashed", b: 1}, expectMetrics: true},
    // Test non-compound and compound shard keys with a shard key prefixed index.
    {shardKey: {a: 1}, indexKey: {a: 1, b: 1}, expectMetrics: true},
    {shardKey: {a: 1, b: 1}, indexKey: {a: 1, b: 1, c: 1}, expectMetrics: true},
    {shardKey: {"a.x": 1}, indexKey: {"a.x": 1, b: "hashed"}, expectMetrics: true},
    {shardKey: {"a.x.y": "hashed"}, indexKey: {"a.x.y": "hashed", b: 1}, expectMetrics: true},
    // Test shard keys with _id.
    {shardKey: {_id: 1}, indexKey: {_id: 1}, expectMetrics: true},
    {shardKey: {_id: 1, a: 1}, indexKey: {_id: 1, a: 1}, expectMetrics: true},
    // Test shard key indexes with simple collation.
    {
        shardKey: {a: 1},
        indexKey: {a: 1},
        indexOptions: {collation: {locale: "simple"}},
        expectMetrics: true
    },
];

const compatibleIndexTestCases = [
    // Test non-compound and compound shard keys with a compatible index.
    {shardKey: {a: 1}, indexKey: {a: "hashed"}, expectMetrics: true},
    {shardKey: {a: "hashed"}, indexKey: {a: 1}, expectMetrics: true},
    {shardKey: {"a.x": 1, b: "hashed"}, indexKey: {"a.x": "hashed", b: 1}, expectMetrics: true},
    {shardKey: {"a.x.y": "hashed", b: 1}, indexKey: {"a.x.y": 1, b: "hashed"}, expectMetrics: true},
    {shardKey: {a: 1, b: 1}, indexKey: {a: 1, b: "hashed", c: 1}, expectMetrics: true},
    // Test shard keys with _id.
    {shardKey: {_id: "hashed"}, indexKey: {_id: 1}, expectMetrics: true},
    // Test shard key indexes with simple collation.
    {
        shardKey: {a: 1},
        indexKey: {a: "hashed"},
        indexOptions: {collation: {locale: "simple"}},
        expectMetrics: true
    },
];

const noIndexTestCases = [
    // Test non-compound and compound shard keys without a shard key prefixed index or a compatible
    // index.
    {shardKey: {a: 1}, expectMetrics: false},
    {shardKey: {a: 1, b: 1}, indexKey: {b: 1}, expectMetrics: false},
    {shardKey: {a: 1, b: 1}, indexKey: {a: 1, c: 1}, expectMetrics: false},
    {
        shardKey: {a: 1},
        indexKey: {a: 1},
        indexOptions: {collation: {locale: "fr"}},  // non-simple collation.
    },
    {
        shardKey: {a: 1},
        indexKey: {a: 1},
        indexOptions: {sparse: true},
    },
    {
        shardKey: {a: 1},
        indexKey: {a: 1},
        indexOptions: {partialFilterExpression: {a: {$gte: 1}}},
    },
];

// Construct test cases from the base test cases above.
const candidateKeyTestCases = [];
const currentKeyTestCases = [];

for (let testCaseBase of shardKeyPrefixedIndexTestCases) {
    if (!AnalyzeShardKeyUtil.isIdKeyPattern(testCaseBase.indexKey)) {
        const testCase = Object.extend({indexOptions: {}}, testCaseBase, true /* deep */);
        testCase.indexOptions.unique = false;
        testCase.expectUnique = false;
        candidateKeyTestCases.push(testCase);
        currentKeyTestCases.push(testCase);
    }

    if (!AnalyzeShardKeyUtil.isHashedKeyPattern(testCaseBase.indexKey)) {
        // Hashed indexes cannot have a uniqueness constraint.
        const testCase = Object.extend({indexOptions: {}}, testCaseBase, true /* deep */);
        testCase.indexOptions.unique = true;
        testCase.expectUnique =
            Object.keys(testCaseBase.shardKey).length == Object.keys(testCaseBase.indexKey).length;
        candidateKeyTestCases.push(testCase);
        currentKeyTestCases.push(testCase);
    }
}
for (let testCaseBase of compatibleIndexTestCases) {
    if (!AnalyzeShardKeyUtil.isIdKeyPattern(testCaseBase.indexKey)) {
        const testCase = Object.extend({indexOptions: {}}, testCaseBase, true /* deep */);
        testCase.indexOptions.unique = false;
        testCase.expectUnique = false;
        candidateKeyTestCases.push(testCase);
    }

    if (!AnalyzeShardKeyUtil.isHashedKeyPattern(testCaseBase.indexKey)) {
        // Hashed indexes cannot have a uniqueness constraint.
        const testCase = Object.extend({indexOptions: {}}, testCaseBase, true /* deep */);
        testCase.indexOptions.unique = true;
        testCase.expectUnique =
            Object.keys(testCaseBase.shardKey).length == Object.keys(testCaseBase.indexKey).length;
        candidateKeyTestCases.push(testCase);
    }
}
for (let testCaseBase of noIndexTestCases) {
    // No metrics are expected for these test cases so there is no need to test with both non-unique
    // and unique index.
    const testCase = Object.extend({indexOptions: {}}, testCaseBase, true /* deep */);
    testCase.indexOptions.unique = false;
    candidateKeyTestCases.push(testCase);
}

function assertNoMetrics(res) {
    assert(!res.hasOwnProperty("numDocs"), res);
    assert(!res.hasOwnProperty("isUnique"), res);
    assert(!res.hasOwnProperty("cardinality"), res);
    assert(!res.hasOwnProperty("frequency"), res);
    assert(!res.hasOwnProperty("monotonicity"), res);
}

function assertMetrics(res, {numDocs, isUnique, cardinality, frequency}) {
    assert.eq(res.numDocs, numDocs, res);
    assert.eq(res.isUnique, isUnique, res);
    assert.eq(res.cardinality, cardinality, res);
    assert.eq(bsonWoCompare(res.frequency, frequency), 0, res);
    assert(res.hasOwnProperty("monotonicity"), res);
}

/**
 * Finds the profiler entries for all aggregate and count commands with the given comment on the
 * given mongods and verifies that:
 * - The aggregate commands used index scan and did not fetch any documents.
 * - The count commands used fast count, i.e. did not scan the index or fetch any documents.
 */
function assertReadQueryPlans(mongodConns, dbName, collName, comment) {
    mongodConns.forEach(conn => {
        const profilerColl = conn.getDB(dbName).system.profile;

        profilerColl.find({"command.aggregate": collName, "command.comment": comment})
            .forEach(doc => {
                if (doc.hasOwnProperty("ok") && (doc.ok === 0)) {
                    return;
                }

                const isMerge = doc.command.pipeline[0].hasOwnProperty("$mergeCursors");
                if (!isMerge) {
                    assert(doc.hasOwnProperty("planSummary"), doc);
                    assert(doc.planSummary.includes("IXSCAN"), doc);
                }
                assert(!doc.usedDisk, doc);
                // Verify that it did not fetch any documents.
                assert.eq(doc.docsExamined, 0, doc);
                // Verify that it opted out of shard filtering.
                assert.eq(doc.readConcern.level, "available", doc);
            });

        profilerColl.find({"command.count": collName, "command.comment": comment}).forEach(doc => {
            if (doc.hasOwnProperty("ok") && (doc.ok === 0)) {
                return;
            }

            assert(doc.hasOwnProperty("planSummary"), doc);
            assert(doc.planSummary.includes("RECORD_STORE_FAST_COUNT"), doc);
            assert(!doc.usedDisk, doc);
            // Verify that it did not scan the index or fetch any documents.
            assert.eq(doc.keysExamined, 0, doc);
            assert.eq(doc.docsExamined, 0, doc);
        });
    });
}

function makeDocument(fieldNames, val) {
    const doc = {};
    fieldNames.forEach(fieldName => {
        if (fieldName == "_id") {
            // The _id must be unique so should be manually set to 'val'.
            return;
        }
        AnalyzeShardKeyUtil.setDottedField(doc, fieldName, val);
    });
    return doc;
}

/**
 * Tests the cardinality and frequency metrics for a shard key that either has a non-unique
 * supporting/compatible index or doesn't a supporting/compatible index.
 */
function testAnalyzeShardKeyNoUniqueIndex(conn, dbName, collName, currentShardKey, testCase) {
    assert(!testCase.indexOptions.unique);

    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    const fieldNames = AnalyzeShardKeyUtil.getCombinedFieldNames(
        currentShardKey, testCase.shardKey, testCase.indexKey);
    const shardKeyContainsId = testCase.shardKey.hasOwnProperty("_id");

    // Analyze the shard key while the collection has less than 5 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(db.runCommand({
        insert: collName,
        documents: [makeDocument(fieldNames, -1), makeDocument(fieldNames, 1)],
        ordered: false
    }));
    let res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    if (testCase.expectMetrics) {
        assertMetrics(res, {
            numDocs: 2,
            isUnique: false,
            cardinality: 2,
            frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
        });
    } else {
        assertNoMetrics(res);
    }

    // Analyze the shard key while the collection has exactly 5 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(db.runCommand({
        insert: collName,
        documents: [
            makeDocument(fieldNames, -2),
            makeDocument(fieldNames, -1),
            makeDocument(fieldNames, 0),
            makeDocument(fieldNames, 1),
            makeDocument(fieldNames, 2)
        ],
        ordered: false
    }));
    res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    if (testCase.expectMetrics) {
        assertMetrics(res, {
            numDocs: 5,
            isUnique: false,
            cardinality: 5,
            frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
        });
    } else {
        assertNoMetrics(res);
    }

    // Analyze the shard key the collection has exactly 100 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    let docs = [];
    let sign = 1;
    for (let i = 1; i <= 100; i++) {
        for (let j = 1; j <= i; j++) {
            docs.push(makeDocument(fieldNames, sign * i));
        }
        sign *= -1;
    }
    assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: false}));
    res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    if (testCase.expectMetrics) {
        if (shardKeyContainsId) {
            // The shard key contains the _id so each document has its own shard key value.
            assertMetrics(res, {
                numDocs: 5050,
                isUnique: false,
                cardinality: 5050,
                frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
            });
        } else {
            assertMetrics(res, {
                numDocs: 5050,
                isUnique: false,
                cardinality: 100,
                frequency: {p99: 99, p95: 95, p90: 90, p80: 80, p50: 50}
            });
        }
    } else {
        assertNoMetrics(res);
    }

    // Analyze the shard key the collection has more than 100 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    docs = [];
    sign = 1;
    for (let i = 1; i <= 150; i++) {
        for (let j = 1; j <= i; j++) {
            docs.push(makeDocument(fieldNames, sign * i));
        }
        sign *= -1;
    }
    assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: false}));
    res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    if (testCase.expectMetrics) {
        if (shardKeyContainsId) {
            // The shard key contains the _id so each document has its own shard key value.
            assertMetrics(res, {
                numDocs: 11325,
                isUnique: false,
                cardinality: 11325,
                frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
            });
        } else {
            assertMetrics(res, {
                numDocs: 11325,
                isUnique: false,
                cardinality: 150,
                frequency: {p99: 149, p95: 143, p90: 135, p80: 120, p50: 75}
            });
        }
    } else {
        assertNoMetrics(res);
    }

    assert.commandWorked(coll.remove({}));
}

/**
 * Tests the cardinality and frequency metrics for a shard key that has a unique
 * supporting/compatible index.
 */
function testAnalyzeShardKeyUniqueIndex(conn, dbName, collName, currentShardKey, testCase) {
    assert(testCase.indexOptions.unique);
    assert(testCase.expectMetrics);

    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    const fieldNames = AnalyzeShardKeyUtil.getCombinedFieldNames(
        currentShardKey, testCase.shardKey, testCase.indexKey);

    // Analyze the shard key while the collection has less than 5 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(db.runCommand({
        insert: collName,
        documents: [makeDocument(fieldNames, -1), makeDocument(fieldNames, 1)],
        ordered: false
    }));
    let res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    assertMetrics(res, {
        numDocs: 2,
        isUnique: testCase.expectUnique,
        cardinality: 2,
        frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
    });

    // Analyze the shard key while the collection has exactly 5 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(db.runCommand({
        insert: collName,
        documents: [
            makeDocument(fieldNames, -2),
            makeDocument(fieldNames, -1),
            makeDocument(fieldNames, 0),
            makeDocument(fieldNames, 1),
            makeDocument(fieldNames, 2)
        ],
        ordered: false
    }));
    res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    assertMetrics(res, {
        numDocs: 5,
        isUnique: testCase.expectUnique,
        cardinality: 5,
        frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
    });

    // Analyze the shard key the collection has exactly 100 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    let docs = [];
    let sign = 1;
    for (let i = 1; i <= 100; i++) {
        docs.push(makeDocument(fieldNames, sign * i));
        sign *= -1;
    }
    assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: false}));
    res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    assertMetrics(res, {
        numDocs: 100,
        isUnique: testCase.expectUnique,
        cardinality: 100,
        frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
    });

    // Analyze the shard key the collection has more than 100 distinct shard key values.
    assert.commandWorked(coll.remove({}));
    docs = [];
    sign = 1;
    for (let i = 1; i <= 150; i++) {
        docs.push(makeDocument(fieldNames, sign * i));
        sign *= -1;
    }
    assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: false}));
    res = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    assertMetrics(res, {
        numDocs: 150,
        isUnique: testCase.expectUnique,
        cardinality: 150,
        frequency: {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}
    });

    assert.commandWorked(coll.remove({}));
}

function testAnalyzeCandidateShardKeysUnshardedCollection(conn, mongodConns) {
    const dbName = "testDbCandidateUnsharded";
    const collName = "testColl";
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    jsTest.log(
        `Testing candidate shard keys for an unsharded collection: ${tojson({dbName, collName})}`);

    candidateKeyTestCases.forEach(testCaseBase => {
        const testCase = Object.assign({}, testCaseBase);
        // Used to identify the operations performed by the analyzeShardKey commands in this test
        // case.
        testCase.comment = UUID();
        jsTest.log(`Testing metrics for ${tojson({dbName, collName, testCase})}`);

        if (testCase.indexKey && !AnalyzeShardKeyUtil.isIdKeyPattern(testCase.indexKey)) {
            assert.commandWorked(coll.createIndex(testCase.indexKey, testCase.indexOptions));
        }
        AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

        if (testCase.indexOptions.unique) {
            testAnalyzeShardKeyUniqueIndex(
                conn, dbName, collName, null /* currentShardKey */, testCase);
        } else {
            testAnalyzeShardKeyNoUniqueIndex(
                conn, dbName, collName, null /* currentShardKey */, testCase);
        }

        AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
        assertReadQueryPlans(mongodConns, dbName, collName, testCase.comment);
        if (testCase.indexKey && !AnalyzeShardKeyUtil.isIdKeyPattern(testCase.indexKey)) {
            assert.commandWorked(coll.dropIndex(testCase.indexKey));
        }
    });

    assert.commandWorked(db.dropDatabase());
}

function testAnalyzeCandidateShardKeysShardedCollection(st, mongodConns) {
    const dbName = "testDbCandidateSharded";
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const currentShardKey = {skey: 1};
    const currentShardKeySplitPoint = {skey: 0};
    const db = st.s.getDB(dbName);
    const coll = db.getCollection(collName);

    jsTest.log(
        `Testing candidate shard keys for a sharded collection: ${tojson({dbName, collName})}`);

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentShardKey}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: currentShardKeySplitPoint}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: currentShardKeySplitPoint, to: st.shard1.shardName}));

    candidateKeyTestCases.forEach(testCaseBase => {
        if (!AnalyzeShardKeyUtil.isIdKeyPattern(testCaseBase.indexKey)) {
            return;
        }

        const testCase = Object.assign({}, testCaseBase);
        if (currentShardKey && testCase.indexOptions.unique) {
            // It is illegal to create a unique index that doesn't have the shard key as a prefix.
            assert(testCase.indexKey);
            testCase.shardKey = Object.assign({}, currentShardKey, testCase.shardKey);
            testCase.indexKey = Object.assign({}, currentShardKey, testCase.indexKey);
        }
        // Used to identify the operations performed by the analyzeShardKey commands in this test
        // case.
        testCase.comment = UUID();
        jsTest.log(`Testing metrics for ${tojson({dbName, collName, currentShardKey, testCase})}`);

        if (testCase.indexKey && !AnalyzeShardKeyUtil.isIdKeyPattern(testCase.indexKey)) {
            assert.commandWorked(coll.createIndex(testCase.indexKey, testCase.indexOptions));
        }
        AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

        if (testCase.indexOptions.unique) {
            testAnalyzeShardKeyUniqueIndex(st.s, dbName, collName, currentShardKey, testCase);
        } else {
            testAnalyzeShardKeyNoUniqueIndex(st.s, dbName, collName, currentShardKey, testCase);
        }

        AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
        assertReadQueryPlans(mongodConns, dbName, collName, testCase.comment);
        if (testCase.indexKey && !AnalyzeShardKeyUtil.isIdKeyPattern(testCase.indexKey)) {
            assert.commandWorked(coll.dropIndex(testCase.indexKey));
        }
    });

    assert.commandWorked(db.dropDatabase());
}

function testAnalyzeCurrentShardKeys(st, mongodConns) {
    const dbName = "testDbCurrentSharded";
    const db = st.s.getDB(dbName);

    jsTest.log(`Testing current shard key for sharded collections: ${tojson({dbName})}`);

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);

    let testNum = 0;
    currentKeyTestCases.forEach(testCaseBase => {
        const testCase = Object.assign({}, testCaseBase);
        // Used to identify the operations performed by the analyzeShardKey commands in this test
        // case.
        testCase.comment = UUID();

        const collName = "testColl-" + testNum++;
        const ns = dbName + "." + collName;
        const currentShardKey = testCase.shardKey;
        const coll = st.s.getCollection(ns);

        jsTest.log(`Testing metrics for ${tojson({dbName, collName, currentShardKey, testCase})}`);

        if (!AnalyzeShardKeyUtil.isIdKeyPattern(testCase.indexKey)) {
            assert.commandWorked(coll.createIndex(testCase.indexKey, testCase.indexOptions));
        }

        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentShardKey}));
        if (!AnalyzeShardKeyUtil.isHashedKeyPattern(currentShardKey)) {
            let shardKeySplitPoint = {};
            for (let fieldName in currentShardKey) {
                shardKeySplitPoint[fieldName] = 0;
            }
            assert.commandWorked(st.s.adminCommand({split: ns, middle: shardKeySplitPoint}));
            assert.commandWorked(st.s.adminCommand(
                {moveChunk: ns, find: shardKeySplitPoint, to: st.shard1.shardName}));
        }

        AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

        if (testCase.indexOptions.unique) {
            testAnalyzeShardKeyUniqueIndex(st.s, dbName, collName, currentShardKey, testCase);
        } else {
            testAnalyzeShardKeyNoUniqueIndex(st.s, dbName, collName, currentShardKey, testCase);
        }

        AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
        assertReadQueryPlans(mongodConns, dbName, collName, testCase.comment);
    });

    assert.commandWorked(db.dropDatabase());
}

{
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 2,
            setParameter: {
                "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
                    tojson({mode: "alwaysOn"})
            }
        }
    });
    const mongodConns = [];
    st.rs0.nodes.forEach(node => mongodConns.push(node));
    st.rs1.nodes.forEach(node => mongodConns.push(node));

    testAnalyzeCandidateShardKeysUnshardedCollection(st.s, mongodConns);
    testAnalyzeCandidateShardKeysShardedCollection(st, mongodConns);
    testAnalyzeCurrentShardKeys(st, mongodConns);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const mongodConns = rst.nodes;

    testAnalyzeCandidateShardKeysUnshardedCollection(rst.getPrimary(), mongodConns);

    rst.stopSet();
}
})();
