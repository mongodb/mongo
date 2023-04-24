/**
 * Test a $group query which has a large number of group-by fields and needs to spill to disk.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const MEM_LIMIT_KB = 2;

// Make sure that we can handle more than 32 keys (the maximum allowed number of components in a
// compound index).
const NUM_GROUP_KEYS = 33;

// Run a mongod that has a reduced memory limit for when its hash aggregation operators (in both
// SBE and the Classic execution engine) will spill data to disk.
const memLimit = MEM_LIMIT_KB * 1024;
const conn = MongoRunner.runMongod({
    setParameter: {
        internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: memLimit,
        internalDocumentSourceGroupMaxMemoryBytes: memLimit
    }
});
assert.neq(conn, null, "mongod failed to start up");

const db = conn.getDB("test");
const coll = db.group_spill_long_keys;

function nextFieldName(name) {
    function nextChar(char) {
        return String.fromCharCode(char.charCodeAt(0) + 1);
    }

    function lastChar(str) {
        return str[str.length - 1];
    }

    // If the final character is a "z", start using a longer string. Otherwise we cycle through all
    // possibilities for the last letter. These means we generate only 26 unique names for each
    // string length, but that's ok since this function will not be used to generate more than ~40
    // unique names.
    if (lastChar(name) === "z") {
        return "a".repeat(name.length + 1);
    } else {
        return name.substr(0, name.length - 1) + nextChar(lastChar(name));
    }
}

let counter = 0;

/**
 * Generates a document with 'NUM_GROUP_KEYS' uniquely named keys. Values are increasingly large
 * 64-bit integers.
 */
function generateDoc() {
    let doc = {};
    let str = "a";
    for (let i = 0; i < NUM_GROUP_KEYS; ++i) {
        doc[str] = NumberLong(counter);
        ++counter;
        str = nextFieldName(str);
    }
    return doc;
}

// Calculate how many documents we need. We use 100 times the approximate number of documents that
// would cause a spill limit in order to cause the query to spill frequently.
let exampleDoc = generateDoc();
let docSize = Object.bsonsize(exampleDoc);
let docsNeeded = Math.ceil(memLimit / docSize) * 100;

coll.drop();
for (let i = 0; i < docsNeeded; ++i) {
    assert.commandWorked(coll.insert(generateDoc()));
}

/**
 * Generates the _id field for a $group query that aggregates on 'NUM_GROUP_KEY' unique keys. The
 * returned document should look like {a: "$a", b: "$b", ...}.
 */
const groupKey = (function() {
    let doc = {};
    let str = "a";
    for (let i = 0; i < NUM_GROUP_KEYS; ++i) {
        doc[str] = "$" + str;
        str = nextFieldName(str);
    }
    return doc;
}());

const pipeline = [{$group: {_id: groupKey}}];

// Run the query twice and assert that there are as many groups as documents in the collection,
// since each document has a unique group key. We run the query twice because the second time it may
// use a cached plan.
for (let i = 0; i < 2; ++i) {
    assert.eq(docsNeeded, coll.aggregate(pipeline).itcount());
}

// Run an explain. If SBE was used, make sure that we see a "group" stage that spilled in the exec
// stats.
let explain = coll.explain("executionStats").aggregate(pipeline);
assert(explain.hasOwnProperty("explainVersion"), explain);
if (explain.explainVersion !== "1") {
    let hashAgg = getPlanStage(explain.executionStats.executionStages, "group");
    // There should be a group-by slot for each field we are grouping by.
    assert.eq(hashAgg.groupBySlots.length, NUM_GROUP_KEYS, hashAgg);
    assert.eq(hashAgg.usedDisk, true, hashAgg);
    assert.gt(hashAgg.spills, 0, hashAgg);
    assert.gt(hashAgg.spilledRecords, 0, hashAgg);
}

MongoRunner.stopMongod(conn);
}());
