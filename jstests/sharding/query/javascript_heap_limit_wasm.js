/**
 * WASM and Native MozJS have different memory tracking mechanisms causing this split.
 * @tags: [
 *   requires_mozjs_wasm,
 *   requires_scripting,
 *   # This test has different behavior between the WAsm and Native MozJS engine.
 *   requires_fcv_90,
 * ]
 */

// Confirms that JavaScript heap limits are respected in aggregation. Includes testing for mapReduce
// and $where which use aggregation for execution.
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const mongos = st.s;

// OOM thresholds are unreliable on debug builds: the WASM runtime's per-object overhead is higher,
// so neither the "too small" nor "sufficient" heap sizes produce consistent results.
if (mongos.adminCommand("buildInfo").debug) {
    jsTestLog(
        "Skipping javascript_heap_limit_wasm.js: OOM thresholds are unreliable on debug " +
            "builds due to higher WASM memory overhead.",
    );
    st.stop();
    quit();
}

let mongosDB = mongos.getDB("test");
let mongosColl = mongosDB.coll;

// Shard the collection with one chunk per shard. Insert a single document into each shard.
st.shardColl(mongosColl.getName(), {x: 1}, {x: 2}, {x: 2});
assert.commandWorked(mongosColl.insert([{x: 0}, {x: 2}]));

// The limits chosen in this test for "tooSmallHeapSizeMB", "sufficentHeapSizeMB" and "arraySize"
// reflect a setup where allocating a string of size "arraySize" with a "tooSmallHeapSizeMB"
// JavaScript heap limit will trigger an OOM event, whereas allocating the same array with a
// "sufficentHeapSizeMB" JavaScript heap limit will succeed.
const tooSmallHeapSizeMB = 50;
const sufficentHeapSizeMB = 200;

const expectedOOMErrorCodes = [ErrorCodes.JSInterpreterFailure, ErrorCodes.ExceededMemoryLimit];

function setHeapSizeLimitMBOneNode({db, queryLimit, globalLimit}) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryJavaScriptHeapSizeLimitMB: queryLimit}),
    );
    assert.commandWorked(db.adminCommand({setParameter: 1, jsHeapLimitMB: globalLimit}));
}

function setHeapSizeLimitMB({queryLimit, globalLimit}) {
    st.forEachMongos((conn) => {
        setHeapSizeLimitMBOneNode({
            db: conn.getDB("test"),
            queryLimit: queryLimit,
            globalLimit: globalLimit,
        });
    });
    st.forEachConnection((conn) => {
        setHeapSizeLimitMBOneNode({
            db: conn.getDB("test"),
            queryLimit: queryLimit,
            globalLimit: globalLimit,
        });
    });
}

// MozJS-WAsm uses a different memory tracking mechanism requiring different scenarios to OOM.
function allocateLargeObject() {
    let head = null;
    for (let i = 0; i < 1_750_000; i++) {
        head = {next: head, v: i};
    }
    return true;
}

const mapReduce = {
    mapReduce: "coll",
    map: allocateLargeObject,
    reduce: function (k, v) {
        return 1;
    },
    out: {inline: 1},
};
const aggregateWithJSFunction = {
    aggregate: "coll",
    cursor: {},
    pipeline: [
        {$group: {_id: "$x"}},
        {
            $project: {
                y: {"$function": {args: [], body: allocateLargeObject, lang: "js"}},
            },
        },
    ],
    allowDiskUse: false,
};
const aggregateWithInternalJsReduce = {
    aggregate: "coll",
    cursor: {},
    pipeline: [
        {
            $group: {
                _id: "$x",
                value: {
                    $_internalJsReduce: {
                        data: {k: "$x", v: "$x"},
                        eval: allocateLargeObject,
                    },
                },
            },
        },
    ],
    allowDiskUse: false,
};
const aggregateWithUserDefinedAccumulator = {
    aggregate: "coll",
    cursor: {},
    pipeline: [
        {
            $group: {
                _id: "$x",
                value: {
                    $accumulator: {
                        init: allocateLargeObject,
                        accumulate: allocateLargeObject,
                        accumulateArgs: [{k: "$x", v: "$x"}],
                        merge: allocateLargeObject,
                        lang: "js",
                    },
                },
            },
        },
    ],
    allowDiskUse: false,
};
const findWithJavaScriptFunction = {
    find: "coll",
    filter: {
        $expr: {"$function": {args: [], body: allocateLargeObject, lang: "js"}},
    },
};
const findWithWhere = {
    find: "coll",
    filter: {$where: allocateLargeObject},
};

/**
 * The following tests will execute JavaScript on the process represented by 'db' regardless of
 * whether it is a mongod or mongos. This is because the JavaScript expressions live in the merger
 * part of an aggregation pipeline.
 */
function runCommonTests(db) {
    // All commands are expected to work with a sufficient JS heap size.
    setHeapSizeLimitMB({db: db, queryLimit: sufficentHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandWorked(db.runCommand(aggregateWithJSFunction));
    assert.commandWorked(db.runCommand(aggregateWithInternalJsReduce));
    assert.commandWorked(db.runCommand(aggregateWithUserDefinedAccumulator));

    // The aggregate command is expected to fail when the aggregation specific heap size limit is
    // too low.
    setHeapSizeLimitMB({db: db, queryLimit: tooSmallHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandFailedWithCode(db.runCommand(aggregateWithJSFunction), expectedOOMErrorCodes);
    assert.commandFailedWithCode(
        db.runCommand(aggregateWithInternalJsReduce),
        expectedOOMErrorCodes,
    );
    assert.commandFailedWithCode(
        db.runCommand(aggregateWithUserDefinedAccumulator),
        expectedOOMErrorCodes,
    );

    // All commands are expected to fail when the global heap size limit is too low, regardless
    // of the aggregation limit.
    setHeapSizeLimitMB({db: db, queryLimit: sufficentHeapSizeMB, globalLimit: tooSmallHeapSizeMB});
    assert.commandFailedWithCode(db.runCommand(aggregateWithJSFunction), expectedOOMErrorCodes);
    assert.commandFailedWithCode(
        db.runCommand(aggregateWithInternalJsReduce),
        expectedOOMErrorCodes,
    );
    assert.commandFailedWithCode(
        db.runCommand(aggregateWithUserDefinedAccumulator),
        expectedOOMErrorCodes,
    );
}

/**
 * The following tests will execute JavaScript only on mongod. This is because $where and $expr are
 * only evaluated on mongod, not on mongos.
 */
function runShardTests(db) {
    // All commands are expected to work with a sufficient JS heap size.
    setHeapSizeLimitMB({db: db, queryLimit: sufficentHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandWorked(db.runCommand(findWithJavaScriptFunction));
    assert.commandWorked(db.runCommand(findWithWhere));
    assert.commandWorked(db.runCommand(mapReduce));

    // A find command with JavaScript agg expression is expected to fail when the query specific
    // heap size limit is too low.
    setHeapSizeLimitMB({db: db, queryLimit: tooSmallHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandFailedWithCode(
        db.runCommand(findWithJavaScriptFunction),
        ErrorCodes.JSInterpreterFailure,
    );

    // The mapReduce command and $where are not limited by the query heap size limit and will
    // succeed even if it is set too low.
    setHeapSizeLimitMB({db: db, queryLimit: tooSmallHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandWorked(db.runCommand(mapReduce));
    assert.commandWorked(db.runCommand(findWithWhere));

    // All commands are expected to fail when the global heap size limit is too low, regardless
    // of the aggregation limit.
    setHeapSizeLimitMB({db: db, queryLimit: sufficentHeapSizeMB, globalLimit: tooSmallHeapSizeMB});
    assert.commandFailedWithCode(
        db.runCommand(findWithJavaScriptFunction),
        ErrorCodes.JSInterpreterFailure,
    );
    assert.commandFailedWithCode(db.runCommand(findWithWhere), ErrorCodes.JSInterpreterFailure);
    assert.commandFailedWithCode(db.runCommand(mapReduce), ErrorCodes.JSInterpreterFailure);
}

// Test command invocations that can execute JavaScript on either mongos or mongod.
runCommonTests(mongosDB);

// Test command invocations that will only execute JavaScript on the shards.
const shardDB = st.shard0.getDB("test");
runCommonTests(shardDB);
runShardTests(shardDB);

st.stop();
