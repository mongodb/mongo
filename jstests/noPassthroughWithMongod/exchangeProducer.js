/**
 * Basic exchange producer tests. We test various document distribution policies.
 */

// This test runs a getMore in a parallel shell, which will not inherit the implicit session of
// the cursor establishing command.
TestData.disableImplicitSessions = true;

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

const coll = db.testCollection;
coll.drop();

const numDocs = 10000;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; ++i) {
    bulk.insert({a: i, b: 'abcdefghijklmnopqrstuvxyz', c: {d: i}, e: [0, {f: i}]});
}

assert.commandWorked(bulk.execute());

/**
 * A consumer runs in a parallel shell reading the cursor until exhausted and then asserts that
 * it got the correct number of documents.
 *
 * @param {Object} cursor - the cursor that a consumer will read
 * @param {int} count - number of expected documents
 */
function countingConsumer(cursor, count) {
    let shell = startParallelShell(`{
            const dbCursor = new DBCommandCursor(db, ${tojsononeline(cursor)});

            assert.eq(${count}, dbCursor.itcount())
        }`);

    return shell;
}

/**
 * A consumer runs in a parallel shell reading the cursor expecting an error.
 *
 * @param {Object} cursor - the cursor that a consumer will read
 * @param {int} code - the expected error code
 */
function failingConsumer(cursor, code) {
    let shell = startParallelShell(`{
            const dbCursor = new DBCommandCursor(db, ${tojsononeline(cursor)});
            const cmdRes = db.runCommand({getMore: dbCursor._cursorid, collection: dbCursor._collName});
            assert.commandFailedWithCode(cmdRes, ${code});
        }`);

    return shell;
}

const numConsumers = 4;
// For simplicity we assume that we can evenly distribute documents among consumers.
assert.eq(0, numDocs % numConsumers);

(function testParameterValidation() {
    const tooManyConsumers = 101;
    assertErrorCode(coll, [], 50950, "Expected too many consumers", {
        exchange: {
            policy: "roundrobin",
            consumers: NumberInt(tooManyConsumers),
            bufferSize: NumberInt(1024)
        },
        cursor: {batchSize: 0}
    });

    const bufferTooLarge = 200 * 1024 * 1024;  // 200 MB
    assertErrorCode(coll, [], 50951, "Expected buffer too large", {
        exchange: {
            policy: "roundrobin",
            consumers: NumberInt(numConsumers),
            bufferSize: NumberInt(bufferTooLarge)
        },
        cursor: {batchSize: 0}
    });
})();

/**
 * RoundRobin - evenly distribute documents to consumers.
 */
(function testRoundRobin() {
    let res = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [],
        exchange:
            {policy: "roundrobin", consumers: NumberInt(numConsumers), bufferSize: NumberInt(1024)},
        cursor: {batchSize: 0}
    }));
    assert.eq(numConsumers, res.cursors.length);

    let parallelShells = [];

    for (let i = 0; i < numConsumers; ++i) {
        parallelShells.push(countingConsumer(res.cursors[i], numDocs / numConsumers));
    }
    for (let i = 0; i < numConsumers; ++i) {
        parallelShells[i]();
    }
})();

/**
 * Broadcast - send a document to all consumers.
 */
(function testBroadcast() {
    let res = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [],
        exchange:
            {policy: "broadcast", consumers: NumberInt(numConsumers), bufferSize: NumberInt(1024)},
        cursor: {batchSize: 0}
    }));
    assert.eq(numConsumers, res.cursors.length);

    let parallelShells = [];

    for (let i = 0; i < numConsumers; ++i) {
        parallelShells.push(countingConsumer(res.cursors[i], numDocs));
    }
    for (let i = 0; i < numConsumers; ++i) {
        parallelShells[i]();
    }
})();

/**
 * Range - send documents to consumer based on the range of values of the 'a' field.
 */
(function testRange() {
    let res = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [],
        exchange: {
            policy: "keyRange",
            consumers: NumberInt(numConsumers),
            bufferSize: NumberInt(1024),
            key: {a: 1},
            boundaries: [{a: MinKey}, {a: 2500}, {a: 5000}, {a: 7500}, {a: MaxKey}],
            consumerIds: [NumberInt(0), NumberInt(1), NumberInt(2), NumberInt(3)]
        },
        cursor: {batchSize: 0}
    }));
    assert.eq(numConsumers, res.cursors.length);

    let parallelShells = [];

    for (let i = 0; i < numConsumers; ++i) {
        parallelShells.push(countingConsumer(res.cursors[i], numDocs / numConsumers));
    }
    for (let i = 0; i < numConsumers; ++i) {
        parallelShells[i]();
    }
})();

/**
 * Range with more complex pipeline.
 */
(function testRangeComplex() {
    let res = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$match: {a: {$gte: 5000}}}, {$sort: {a: -1}}, {$project: {_id: 0, b: 0}}],
        exchange: {
            policy: "keyRange",
            consumers: NumberInt(numConsumers),
            bufferSize: NumberInt(1024),
            key: {a: 1},
            boundaries: [{a: MinKey}, {a: 2500}, {a: 5000}, {a: 7500}, {a: MaxKey}],
            consumerIds: [NumberInt(0), NumberInt(1), NumberInt(2), NumberInt(3)]
        },
        cursor: {batchSize: 0}
    }));
    assert.eq(numConsumers, res.cursors.length);

    let parallelShells = [];

    parallelShells.push(countingConsumer(res.cursors[0], 0));
    parallelShells.push(countingConsumer(res.cursors[1], 0));
    parallelShells.push(countingConsumer(res.cursors[2], 2500));
    parallelShells.push(countingConsumer(res.cursors[3], 2500));

    for (let i = 0; i < numConsumers; ++i) {
        parallelShells[i]();
    }
})();

/**
 * Range with a dotted path.
 */
(function testRangeDottedPath() {
    let res = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [],
        exchange: {
            policy: "keyRange",
            consumers: NumberInt(numConsumers),
            bufferSize: NumberInt(1024),
            key: {"c.d": 1},
            boundaries:
                [{"c.d": MinKey}, {"c.d": 2500}, {"c.d": 5000}, {"c.d": 7500}, {"c.d": MaxKey}],
            consumerIds: [NumberInt(0), NumberInt(1), NumberInt(2), NumberInt(3)]
        },
        cursor: {batchSize: 0}
    }));
    assert.eq(numConsumers, res.cursors.length);

    let parallelShells = [];

    for (let i = 0; i < numConsumers; ++i) {
        parallelShells.push(countingConsumer(res.cursors[i], numDocs / numConsumers));
    }
    for (let i = 0; i < numConsumers; ++i) {
        parallelShells[i]();
    }
})();

/**
 * Range with a dotted path and array.
 */
(function testRangeDottedPath() {
    let res = assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: [],
        exchange: {
            policy: "keyRange",
            consumers: NumberInt(numConsumers),
            bufferSize: NumberInt(1024),
            key: {"e.f": 1},
            boundaries:
                [{"e.f": MinKey}, {"e.f": 2500}, {"e.f": 5000}, {"e.f": 7500}, {"e.f": MaxKey}],
            consumerIds: [NumberInt(0), NumberInt(1), NumberInt(2), NumberInt(3)]
        },
        cursor: {batchSize: 0}
    }));
    assert.eq(numConsumers, res.cursors.length);

    let parallelShells = [];

    // The e.f field contains an array and hence the exchange cannot compute the range. Instead
    // it sends all such documents to the consumer 0 by fiat.
    for (let i = 0; i < numConsumers; ++i) {
        parallelShells.push(countingConsumer(res.cursors[i], i == 0 ? numDocs : 0));
    }
    for (let i = 0; i < numConsumers; ++i) {
        parallelShells[i]();
    }
})();

/**
 * Range - simulate an exception in loading the batch.
 */
(function testRangeFailLoad() {
    const kFailPointName = "exchangeFailLoadNextBatch";
    try {
        assert.commandWorked(
            db.adminCommand({configureFailPoint: kFailPointName, mode: "alwaysOn"}));

        let res = assert.commandWorked(db.runCommand({
            aggregate: coll.getName(),
            pipeline: [],
            exchange: {
                policy: "keyRange",
                consumers: NumberInt(numConsumers),
                bufferSize: NumberInt(1024),
                key: {a: 1},
                boundaries: [{a: MinKey}, {a: 2500}, {a: 5000}, {a: 7500}, {a: MaxKey}],
                consumerIds: [NumberInt(0), NumberInt(1), NumberInt(2), NumberInt(3)]
            },
            cursor: {batchSize: 0}
        }));
        assert.eq(numConsumers, res.cursors.length);

        let parallelShells = [];
        failingConsumer(res.cursors[0], ErrorCodes.FailPointEnabled)();

        // After the first consumer sees an error, each subsequent consumer should see an
        // 'ExchangePassthrough' error.
        for (let i = 0; i < numConsumers - 1; ++i) {
            parallelShells.push(
                failingConsumer(res.cursors[i + 1], ErrorCodes.ExchangePassthrough));
        }
        for (let i = 0; i < numConsumers - 1; ++i) {
            parallelShells[i]();
        }
    } finally {
        assert.commandWorked(db.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));
    }
})();
})();
