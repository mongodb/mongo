/**
 * Defines helpers for testing that the analyzeShardKey command returns correct monotonicity
 * metrics.
 */

import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

export const kOrderTypes = [
    {
        name: "constant",
        monotonicity: "not monotonic",
        supportedFieldTypes: ["integer", "string", "date", "objectid", "uuid"]
    },
    {
        name: "fluctuating",
        monotonicity: "not monotonic",
        supportedFieldTypes: ["integer", "string", "date", "uuid"]
    },
    {
        name: "increasing",
        monotonicity: "monotonic",
        supportedFieldTypes: ["integer", "string", "date", "objectid"]
    },

    {
        name: "decreasing",
        monotonicity: "monotonic",
        supportedFieldTypes: ["integer", "string", "date"]
    }
];

// Only use a single-node RS for montonicity tests. By default, the router runs the analyzeShardKey
// command with readPreference "secondaryPreferred", and the noise in insertion-order from parallel
// oplog application on secondaries can cause the monotonicity check on secondaries to return an
// incorrect result.
export const numNodesPerRS = 1;
export const insertBatchSize = 1000;

/**
 * Appends the field of the specified name and type to the given documents such that the field value
 * is identical across the documents.
 */
export function appendConstantField(docs, fieldName, fieldType) {
    const value = (() => {
        switch (fieldType) {
            case "integer":
                return 1;
            case "string":
                return "A1";
            case "date":
                return new Date();
            case "objectid":
                return new ObjectId();
            case "uuid":
                return new UUID();
            default:
                break;
        }
        throw "Unexpected field type";
    })();
    for (let i = 0; i < docs.length; i++) {
        AnalyzeShardKeyUtil.setDottedField(docs[i], fieldName, value);
    }
}

/**
 * Appends the field of the specified name and type to the given documents such that the field value
 * is random (i.e. fluctuating).
 */
export function appendFluctuatingField(docs, fieldName, fieldType) {
    const maxIntValue = docs.length;
    const numDigits = maxIntValue.toString().length;

    for (let i = 0; i < docs.length; i++) {
        const value = (() => {
            const intValue = AnalyzeShardKeyUtil.getRandInteger(0, maxIntValue);
            switch (fieldType) {
                case "integer":
                    return intValue;
                case "string":
                    return "A" + intValue.toString().padStart(numDigits, "0");
                case "date": {
                    let dateValue = new Date(1970, 0, 1);
                    dateValue.setSeconds(intValue);
                    return dateValue;
                }
                case "objectid":
                    throw "Cannot have a fluctuating ObjectId";
                case "uuid":
                    return new UUID();
                default:
                    break;
            }
            throw "Unexpected field type";
        })();
        AnalyzeShardKeyUtil.setDottedField(docs[i], fieldName, value);
    }
}

/**
 * The helper for 'appendIncreasingField()' and 'appendDecreasingField()'. Appends the value
 * returned by 'getNextValueFunc()' to the given documents in order, each value is appended to at
 * most 'maxFrequency' documents.
 */
export function appendDuplicatedField(docs, fieldName, getNextValueFunc, maxFrequency) {
    let docIndex = 0;
    while (docIndex < docs.length) {
        const value = getNextValueFunc();
        const frequency =
            Math.min(docs.length - docIndex, AnalyzeShardKeyUtil.getRandInteger(1, maxFrequency));
        for (let i = 0; i < frequency; i++) {
            AnalyzeShardKeyUtil.setDottedField(docs[docIndex], fieldName, value);
            docIndex++;
        }
    }
}

/**
 * Appends the field of the specified name and type to the given documents such that the field value
 * is increasing and each is duplicated at most 'maxFrequency' times.
 */
export function appendIncreasingField(docs, fieldName, fieldType, maxFrequency) {
    const maxIntValue = docs.length;
    const numDigits = maxIntValue.toString().length;
    let intValue = 0;

    const getNextValueFunc = () => {
        intValue++;
        switch (fieldType) {
            case "integer":
                return intValue;
            case "string":
                return "A" + intValue.toString().padStart(numDigits, "0");
            case "date": {
                let dateValue = new Date(1970, 0, 1);
                dateValue.setSeconds(intValue);
                return dateValue;
            }
            case "objectid":
                return new ObjectId();
            case "uuid":
                throw "Cannot have a increasing UUID";
            default:
                break;
        }
        throw "Unexpected field type";
    };

    appendDuplicatedField(docs, fieldName, getNextValueFunc, maxFrequency);
}

/**
 * Appends the field of the specified name and type to the given documents such that the field value
 * is decreasing and each is duplicated at most 'maxFrequency' times.
 */
export function appendDecreasingField(docs, fieldName, fieldType, maxFrequency) {
    const maxIntValue = docs.length;
    const numDigits = maxIntValue.toString().length;
    let intValue = maxIntValue;

    const getNextValueFunc = () => {
        intValue--;
        switch (fieldType) {
            case "integer":
                return intValue;
            case "string":
                return "A" + intValue.toString().padStart(numDigits, "0");
            case "date": {
                let dateValue = new Date(1970, 0, 1);
                dateValue.setSeconds(intValue);
                return dateValue;
            }
            case "objectid":
                throw "Cannot have a decreasing ObjectId";
            case "uuid":
                throw "Cannot have a decreasing UUID";
            default:
                break;
        }
        throw "Unexpected field type";
    };

    appendDuplicatedField(docs, fieldName, getNextValueFunc, maxFrequency);
}

/**
 * Returns 'numDocs' documents created based on the given 'fieldOpts'.
 */
export function makeDocuments(numDocs, fieldOpts) {
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({});
    }
    for (let {name, type, order, maxFrequency} of fieldOpts) {
        switch (order) {
            case "constant":
                appendConstantField(docs, name, type);
                break;
            case "increasing":
                appendIncreasingField(docs, name, type, maxFrequency);
                break;
            case "decreasing":
                appendDecreasingField(docs, name, type, maxFrequency);
                break;
            case "fluctuating":
                appendFluctuatingField(docs, name, type);
                break;
            default:
                throw "Unexpected field order";
        }
    }
    return docs;
}

export function testMonotonicity(conn,
                                 dbName,
                                 collName,
                                 currentShardKey,
                                 testCases,
                                 testProbability,
                                 numDocsRange,
                                 setupCollection) {
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);

    const correlationCoefficientThreshold =
        assert
            .commandWorked(db.adminCommand(
                {getParameter: 1, analyzeShardKeyMonotonicityCorrelationCoefficientThreshold: 1}))
            .analyzeShardKeyMonotonicityCorrelationCoefficientThreshold;

    testCases.forEach(testCase => {
        if (Math.random() > testProbability) {
            return;
        }
        const numDocs = AnalyzeShardKeyUtil.getRandInteger(numDocsRange.min, numDocsRange.max);
        for (let i = 0; i < testCase.fieldOpts.length; i++) {
            const order = testCase.fieldOpts[i].order;
            if (order == "increasing" || order == "decreasing") {
                // Make the field have at least 15 unique values in the collection.
                testCase.fieldOpts[i].maxFrequency =
                    AnalyzeShardKeyUtil.getRandInteger(1, numDocs / 15);
            }
        }
        const fieldOpts = [...testCase.fieldOpts];
        if (currentShardKey) {
            for (let fieldName in currentShardKey) {
                fieldOpts.push({name: fieldName, type: "integer", order: "fluctuating"});
            }
        }

        jsTest.log(`Testing metrics for ${
            tojson({dbName, collName, currentShardKey, numDocs, testCase})}`);

        setupCollection();

        // To reduce the insertion order noise caused by parallel oplog application on
        // secondaries, insert the documents in multiple batches.
        const docs = makeDocuments(numDocs, fieldOpts);
        let currIndex = 0;
        while (currIndex < docs.length) {
            const endIndex = currIndex + insertBatchSize;
            assert.commandWorked(db.runCommand({
                insert: collName,
                documents: docs.slice(currIndex, endIndex),
                // Wait for secondaries to have replicated the writes.
                writeConcern: {w: numNodesPerRS}
            }));
            currIndex = endIndex;
        }

        // Waiting for the index to be created on all nodes is necessary since mongos runs
        // the analyzeShardKey command with readPreference "secondaryPreferred".
        assert.commandWorked(db.runCommand({
            createIndexes: collName,
            indexes: [{key: testCase.indexKey, name: JSON.stringify(testCase.indexKey)}],
            writeConcern: {w: numNodesPerRS}
        }));

        const res = assert.commandWorked(conn.adminCommand({
            analyzeShardKey: ns,
            key: testCase.shardKey,
            // Skip calculating the read and write distribution metrics since there are not needed
            // by this test.
            readWriteDistribution: false
        }));
        const metrics = res.keyCharacteristics;

        const isClusteredColl = AnalyzeShardKeyUtil.isClusterCollection(conn, dbName, collName);
        const expectedType = isClusteredColl ? "unknown" : testCase.expected;
        assert.eq(metrics.monotonicity.type, expectedType, res);

        if (expectedType == "unknown") {
            assert(!metrics.monotonicity.hasOwnProperty("recordIdCorrelationCoefficient"));
        } else {
            assert(metrics.monotonicity.hasOwnProperty("recordIdCorrelationCoefficient"));

            if (expectedType == "monotonic") {
                assert.gte(Math.abs(metrics.monotonicity.recordIdCorrelationCoefficient),
                           correlationCoefficientThreshold);
            } else if (expectedType == "not monotonic") {
                assert.lt(Math.abs(metrics.monotonicity.recordIdCorrelationCoefficient),
                          correlationCoefficientThreshold);
            } else {
                throw new Error("Unknown expected monotonicity '" + expectedType + "'");
            }
        }

        assert.commandWorked(db.dropDatabase());
        for (let i = 0; i < testCase.fieldOpts.length; i++) {
            delete testCase.fieldOpts[i].maxFrequency;
        }
    });
}

export function testAnalyzeShardKeysUnshardedCollection(
    conn, testCases, testProbability, numDocsRange) {
    const dbName = "testDb";
    const collName = "testCollUnsharded";

    jsTest.log(`Testing analyzing a shard key for an unsharded collection: ${
        tojsononeline({dbName, collName})}`);

    let setUpCollection = () => {};
    testMonotonicity(conn,
                     dbName,
                     collName,
                     null /* currentShardKey */,
                     testCases,
                     testProbability,
                     numDocsRange,
                     setUpCollection);
}

export function testAnalyzeShardKeysShardedCollection(
    st, testCases, testProbability, numDocsRange) {
    const dbName = "testDb";
    const collName = "testCollSharded";
    const ns = dbName + "." + collName;
    const currentShardKey = {skey: 1};
    const currentShardKeySplitPoint = {skey: 0};

    jsTest.log(`Testing analyzing a shard key for a sharded collection: ${
        tojsononeline({dbName, collName})}`);

    let setUpCollection = () => {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentShardKey}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: currentShardKeySplitPoint}));
        assert.commandWorked(st.s.adminCommand(
            {moveChunk: ns, find: currentShardKeySplitPoint, to: st.shard1.shardName}));
    };
    testMonotonicity(st.s,
                     dbName,
                     collName,
                     currentShardKey,
                     testCases,
                     testProbability,
                     numDocsRange,
                     setUpCollection);
}
