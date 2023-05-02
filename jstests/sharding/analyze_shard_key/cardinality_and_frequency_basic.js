/**
 * Tests that the analyzeShardKey command returns correct cardinality and frequency metrics.
 *
 * @tags: [requires_fcv_70]
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

const numNodesPerRS = 2;
const numMostCommonValues = 5;

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

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

                const firstStage = doc.command.pipeline[0];

                if (firstStage.hasOwnProperty("$collStats")) {
                    return;
                }

                if (firstStage.hasOwnProperty("$match") || firstStage.hasOwnProperty("$limit")) {
                    // This corresponds to the aggregation that the analyzeShardKey command runs
                    // to look up documents for a shard key with a unique or hashed supporting
                    // index. For both cases, it should fetch at most 'numMostCommonValues'
                    // documents.
                    assert(doc.hasOwnProperty("planSummary"), doc);
                    assert.lte(doc.docsExamined, numMostCommonValues, doc);
                } else {
                    // This corresponds to the aggregation that the analyzeShardKey command runs
                    // when analyzing a shard key with a non-unique supporting index.
                    if (!firstStage.hasOwnProperty("$mergeCursors")) {
                        assert(doc.hasOwnProperty("planSummary"), doc);
                        assert(doc.planSummary.includes("IXSCAN"), doc);
                    }

                    // Verify that it fetched at most 'numMostCommonValues' documents.
                    assert.lte(doc.docsExamined, numMostCommonValues, doc);
                    // Verify that it opted out of shard filtering.
                    assert.eq(doc.readConcern.level, "available", doc);
                }
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

/**
 * Returns an object where each field name is set to the given value.
 */
function makeDocument(fieldNames, value) {
    const doc = {};
    fieldNames.forEach(fieldName => {
        AnalyzeShardKeyUtil.setDottedField(doc, fieldName, value);
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
    const isUnique = false;

    const makeSubTestCase = (numDistinctValues) => {
        const docs = [];
        const mostCommonValues = [];

        const maxFrequency = shardKeyContainsId ? 1 : numDistinctValues;
        let sign = 1;
        for (let i = 1; i <= numDistinctValues; i++) {
            // Test with integer field half of time and object field half of the time.
            const val = sign * i;
            const doc = makeDocument(fieldNames, Math.random() > 0.5 ? val : {foo: val});

            const frequency = shardKeyContainsId ? 1 : i;
            for (let j = 1; j <= frequency; j++) {
                docs.push(doc);
            }

            const isMostCommon = (maxFrequency - frequency) < numMostCommonValues;
            if (testCase.expectMetrics && isMostCommon) {
                mostCommonValues.push({
                    value: AnalyzeShardKeyUtil.extractShardKeyValueFromDocument(doc,
                                                                                testCase.shardKey),
                    frequency
                });
            }

            sign *= -1;
        }

        const metrics = {
            numDocs: docs.length,
            isUnique,
            numDistinctValues,
            mostCommonValues,
            numMostCommonValues
        };

        return [docs, metrics];
    };

    // Analyze the shard key while the collection has less than 'numMostCommonValues' distinct shard
    // key values.
    const [docs0, metrics0] = makeSubTestCase(numMostCommonValues - 1);
    assert.commandWorked(coll.insert(docs0, {writeConcern}));
    const res0 = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    if (testCase.expectMetrics) {
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res0, metrics0);
    } else {
        AnalyzeShardKeyUtil.assertNotContainKeyCharacteristicsMetrics(res0);
    }
    assert.commandWorked(coll.remove({}));

    // Analyze the shard key while the collection has exactly 'numMostCommonValues' distinct shard
    // key values.
    const [docs1, metrics1] = makeSubTestCase(numMostCommonValues);
    assert.commandWorked(coll.insert(docs1, {writeConcern}));
    const res1 = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    if (testCase.expectMetrics) {
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res1, metrics1);
    } else {
        AnalyzeShardKeyUtil.assertNotContainKeyCharacteristicsMetrics(res1);
    }
    assert.commandWorked(coll.remove({}));

    // Analyze the shard key while the collection has more than 'numMostCommonValues' distinct shard
    // key values.
    const [docs2, metrics2] = makeSubTestCase(numMostCommonValues * 25);
    assert.commandWorked(coll.insert(docs2, {writeConcern}));
    const res2 = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    if (testCase.expectMetrics) {
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res2, metrics2);
    } else {
        AnalyzeShardKeyUtil.assertNotContainKeyCharacteristicsMetrics(res2);
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
    const isUnique = testCase.expectUnique;

    const makeSubTestCase = (numDistinctValues) => {
        const docs = [];
        const mostCommonValues = [];

        let sign = 1;
        for (let i = 1; i <= numDistinctValues; i++) {
            // Test with integer field half of time and object field half of the time.
            const val = sign * i;
            const doc = makeDocument(fieldNames, Math.random() > 0.5 ? val : {foo: val});
            docs.push(doc);
            mostCommonValues.push({
                value: AnalyzeShardKeyUtil.extractShardKeyValueFromDocument(doc, testCase.shardKey),
                frequency: 1
            });

            sign *= -1;
        }

        const metrics = {
            numDocs: docs.length,
            isUnique,
            numDistinctValues,
            mostCommonValues,
            numMostCommonValues
        };

        return [docs, metrics];
    };

    // Analyze the shard key while the collection has less than 'numMostCommonValues' distinct shard
    // key values.
    const [docs0, metrics0] = makeSubTestCase(numMostCommonValues - 1);
    assert.commandWorked(coll.insert(docs0, {writeConcern}));
    const res0 = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res0, metrics0);
    assert.commandWorked(coll.remove({}));

    // Analyze the shard key while the collection has exactly 'numMostCommonValues' distinct shard
    // key values.
    const [docs1, metrics1] = makeSubTestCase(numMostCommonValues);
    assert.commandWorked(coll.insert(docs1, {writeConcern}));
    const res1 = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res1, metrics1);
    assert.commandWorked(coll.remove({}));

    // Analyze the shard key while the collection has more than 'numMostCommonValues' distinct shard
    // key values.
    const [docs2, metrics2] = makeSubTestCase(numMostCommonValues * 25);
    assert.commandWorked(coll.insert(docs2, {writeConcern}));
    const res2 = assert.commandWorked(conn.adminCommand(
        {analyzeShardKey: ns, key: testCase.shardKey, comment: testCase.comment}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res2, metrics2);
    assert.commandWorked(coll.remove({}));
}

function testAnalyzeCandidateShardKeysUnshardedCollection(conn, mongodConns) {
    const dbName = "testDb";
    const collName = "testCollUnshardedCandidate";
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
    const dbName = "testDb";
    const collName = "testCollShardedCandidate";
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
    const dbName = "testDb";
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

        const collName = "testCollShardedCurrent-" + testNum++;
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

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues,
    // Skip calculating the read and write distribution metrics since there are no sampled queries
    // anyway.
    "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
        tojson({mode: "alwaysOn"})
};

{
    const st =
        new ShardingTest({shards: numNodesPerRS, rs: {nodes: 2, setParameter: setParameterOpts}});
    const mongodConns = [];
    st.rs0.nodes.forEach(node => mongodConns.push(node));
    st.rs1.nodes.forEach(node => mongodConns.push(node));

    testAnalyzeCandidateShardKeysUnshardedCollection(st.s, mongodConns);
    testAnalyzeCandidateShardKeysShardedCollection(st, mongodConns);
    testAnalyzeCurrentShardKeys(st, mongodConns);

    st.stop();
}

{
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const mongodConns = rst.nodes;

    testAnalyzeCandidateShardKeysUnshardedCollection(rst.getPrimary(), mongodConns);

    rst.stopSet();
}
})();
