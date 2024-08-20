/**
 * Tests that the analyzeShardKey command returns correct cardinality and frequency metrics when
 * no document sampling is involved.
 *
 * @tags: [requires_fcv_70]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {
    assertAggregateQueryPlans,
    getMongodConns,
    numMostCommonValues
} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common.js";

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
        expectMetrics: false
    },
    {
        shardKey: {a: 1},
        indexKey: {a: 1},
        indexOptions: {sparse: true},
        expectMetrics: false,
    },
    {
        shardKey: {a: 1},
        indexKey: {a: 1},
        indexOptions: {partialFilterExpression: {a: {$gte: 1}}},
        expectMetrics: false
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

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

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
    const res0 = conn.adminCommand({
        analyzeShardKey: ns,
        key: testCase.shardKey,
        comment: testCase.comment,
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    });
    if (testCase.expectMetrics) {
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res0.keyCharacteristics, metrics0);
    } else {
        assert.commandFailedWithCode(res0, ErrorCodes.IllegalOperation);
    }
    assert.commandWorked(coll.remove({}, {writeConcern}));

    // Analyze the shard key while the collection has exactly 'numMostCommonValues' distinct shard
    // key values.
    const [docs1, metrics1] = makeSubTestCase(numMostCommonValues);
    assert.commandWorked(coll.insert(docs1, {writeConcern}));
    const res1 = conn.adminCommand({
        analyzeShardKey: ns,
        key: testCase.shardKey,
        comment: testCase.comment,
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    });
    if (testCase.expectMetrics) {
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res1.keyCharacteristics, metrics1);
    } else {
        assert.commandFailedWithCode(res1, ErrorCodes.IllegalOperation);
    }
    assert.commandWorked(coll.remove({}, {writeConcern}));

    // Analyze the shard key while the collection has more than 'numMostCommonValues' distinct shard
    // key values.
    const [docs2, metrics2] = makeSubTestCase(numMostCommonValues * 25);
    assert.commandWorked(coll.insert(docs2, {writeConcern}));
    const res2 = conn.adminCommand({
        analyzeShardKey: ns,
        key: testCase.shardKey,
        comment: testCase.comment,
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    });
    if (testCase.expectMetrics) {
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res2.keyCharacteristics, metrics2);
    } else {
        assert.commandFailedWithCode(res2, ErrorCodes.IllegalOperation);
    }
    assert.commandWorked(coll.remove({}, {writeConcern}));
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
    const res0 = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: testCase.shardKey,
        comment: testCase.comment,
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res0.keyCharacteristics, metrics0);
    assert.commandWorked(coll.remove({}, {writeConcern}));

    // Analyze the shard key while the collection has exactly 'numMostCommonValues' distinct shard
    // key values.
    const [docs1, metrics1] = makeSubTestCase(numMostCommonValues);
    assert.commandWorked(coll.insert(docs1, {writeConcern}));
    const res1 = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: testCase.shardKey,
        comment: testCase.comment,
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res1.keyCharacteristics, metrics1);
    assert.commandWorked(coll.remove({}, {writeConcern}));

    // Analyze the shard key while the collection has more than 'numMostCommonValues' distinct shard
    // key values.
    const [docs2, metrics2] = makeSubTestCase(numMostCommonValues * 25);
    assert.commandWorked(coll.insert(docs2, {writeConcern}));
    const res2 = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: testCase.shardKey,
        comment: testCase.comment,
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res2.keyCharacteristics, metrics2);
    assert.commandWorked(coll.remove({}, {writeConcern}));
}

function testAnalyzeCandidateShardKeysUnshardedCollection(conn, {rst, st}) {
    const dbName = "testDb";
    const collName = "testCollUnshardedCandidate";
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    const mongodConns = getMongodConns({rst, st});

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
        assertAggregateQueryPlans(
            mongodConns,
            dbName,
            collName,
            testCase.comment,
            // On a replica set, the analyzeShardKey command runs the aggregate commands locally,
            // i.e. the commands do not go through the service entry point so do not get profiled.
            testCase.expectMetrics && !rst /* expectEntries */);
        if (testCase.indexKey && !AnalyzeShardKeyUtil.isIdKeyPattern(testCase.indexKey)) {
            assert.commandWorked(coll.dropIndex(testCase.indexKey));
        }
    });

    assert.commandWorked(db.dropDatabase());
}

function testAnalyzeCandidateShardKeysShardedCollection(st) {
    const dbName = "testDb";
    const collName = "testCollShardedCandidate";
    const ns = dbName + "." + collName;
    const currentShardKey = {skey: 1};
    const currentShardKeySplitPoint = {skey: 0};
    const db = st.s.getDB(dbName);
    const coll = db.getCollection(collName);
    const mongodConns = getMongodConns({st});

    jsTest.log(
        `Testing candidate shard keys for a sharded collection: ${tojson({dbName, collName})}`);

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
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
        assertAggregateQueryPlans(mongodConns,
                                  dbName,
                                  collName,
                                  testCase.comment,
                                  testCase.expectMetrics /* expectEntries */);
        if (testCase.indexKey && !AnalyzeShardKeyUtil.isIdKeyPattern(testCase.indexKey)) {
            assert.commandWorked(coll.dropIndex(testCase.indexKey));
        }
    });

    assert.commandWorked(db.dropDatabase());
}

function testAnalyzeCurrentShardKeys(st) {
    const dbName = "testDb";
    const db = st.s.getDB(dbName);
    const mongodConns = getMongodConns({st});

    jsTest.log(`Testing current shard key for sharded collections: ${tojson({dbName})}`);

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

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
        assertAggregateQueryPlans(mongodConns,
                                  dbName,
                                  collName,
                                  testCase.comment,
                                  testCase.expectMetrics /* expectEntries */);
    });

    assert.commandWorked(db.dropDatabase());
}

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues
};

{
    const st =
        new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    testAnalyzeCandidateShardKeysUnshardedCollection(st.s, {st});
    testAnalyzeCandidateShardKeysShardedCollection(st);
    testAnalyzeCurrentShardKeys(st);

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();

    testAnalyzeCandidateShardKeysUnshardedCollection(rst.getPrimary(), {rst});

    rst.stopSet();
}