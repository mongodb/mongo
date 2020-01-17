// Confirms that JavaScript heap limits are respected in aggregation. Includes testing for mapReduce
// and $where which use aggregation for execution.
// @tags: [requires_fcv_44]
(function() {
"use strict";

const st = new ShardingTest({shards: 2});
const mongos = st.s;

let mongosDB = mongos.getDB("test");
let mongosColl = mongosDB.coll;

// Shard the collection with one chunk per shard. Insert a single document into each shard.
st.shardColl(mongosColl.getName(), {x: 1}, {x: 2}, {x: 2});
assert.commandWorked(mongosColl.insert([{x: 0}, {x: 2}]));

// The limits chosen in this test for "tooSmallHeapSizeMB", "sufficentHeapSizeMB" and "arraySize"
// reflect a setup where allocating a string of size "arraySize" with a "tooSmallHeapSizeMB"
// JavaScript heap limit will trigger an OOM event, whereas allocating the same array with a
// "sufficentHeapSizeMB" JavaScript heap limit will succeed.
const tooSmallHeapSizeMB = 10;
const sufficentHeapSizeMB = 100;

function setHeapSizeLimitMB({db, queryLimit, globalLimit}) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryJavaScriptHeapSizeLimitMB: queryLimit}));
    assert.commandWorked(db.adminCommand({setParameter: 1, jsHeapLimitMB: globalLimit}));
}

function allocateLargeString() {
    const arraySize = 10000000;
    let str = new Array(arraySize).join("x");
    return true;
}

const mapReduce = {
    mapReduce: "coll",
    map: allocateLargeString,
    reduce: function(k, v) {
        return 1;
    },
    out: {inline: 1}
};
const aggregateWithJSFunction = {
    aggregate: "coll",
    cursor: {},
    pipeline: [
        {$group: {_id: "$x"}},
        {$project: {y: {"$function": {args: [], body: allocateLargeString, lang: "js"}}}}
    ]
};
const aggregateWithInternalJsReduce = {
    aggregate: "coll",
    cursor: {},
    pipeline: [{
        $group: {
            _id: "$x",
            value: {
                $_internalJsReduce: {
                    data: {k: "$x", v: "$x"},
                    eval: allocateLargeString,
                }
            }
        }
    }]
};
const aggregateWithUserDefinedAccumulator = {
    aggregate: "coll",
    cursor: {},
    pipeline: [{
        $group: {
            _id: "$x",
            value: {
                $accumulator: {
                    init: allocateLargeString,
                    accumulate: allocateLargeString,
                    accumulateArgs: [{k: "$x", v: "$x"}],
                    merge: allocateLargeString,
                    lang: 'js',
                }
            }
        }
    }]
};
const findWithJavaScriptFunction = {
    find: "coll",
    filter: {$expr: {"$function": {args: [], body: allocateLargeString, lang: "js"}}}
};
const findWithWhere = {
    find: "coll",
    filter: {$where: allocateLargeString}
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
    assert.commandFailedWithCode(db.runCommand(aggregateWithJSFunction),
                                 ErrorCodes.JSInterpreterFailure);
    assert.commandFailedWithCode(db.runCommand(aggregateWithInternalJsReduce),
                                 ErrorCodes.JSInterpreterFailure);
    assert.commandFailedWithCode(db.runCommand(aggregateWithUserDefinedAccumulator),
                                 ErrorCodes.JSInterpreterFailure);

    // All commands are expected to fail when the global heap size limit is too low, regardless
    // of the aggregation limit.
    setHeapSizeLimitMB({db: db, queryLimit: sufficentHeapSizeMB, globalLimit: tooSmallHeapSizeMB});
    assert.commandFailedWithCode(db.runCommand(aggregateWithJSFunction),
                                 ErrorCodes.JSInterpreterFailure);
    assert.commandFailedWithCode(db.runCommand(aggregateWithInternalJsReduce),
                                 ErrorCodes.JSInterpreterFailure);
    assert.commandFailedWithCode(db.runCommand(aggregateWithUserDefinedAccumulator),
                                 ErrorCodes.JSInterpreterFailure);
}

/**
 * The following tests will execute JavaScript only on mongod. This is because $where and $expr are
 * only evaluated on mongod, not on mongos.
 */
function runShardTests(db) {
    // All commands are expected to work with a sufficient JS heap size.
    setHeapSizeLimitMB({db: db, queryLimit: sufficentHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandWorked(db.runCommand(findWithJavaScriptFunction));
    // TODO SERVER-45454: Uncomment when $where is executed via  aggregation JavaScript expression.
    // assert.commandWorked(db.runCommand(findWithWhere));
    assert.commandWorked(db.runCommand(mapReduce));

    // A find command with JavaScript agg expression is expected to fail when the query specific
    // heap size limit is too low.
    setHeapSizeLimitMB({db: db, queryLimit: tooSmallHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandFailedWithCode(db.runCommand(findWithJavaScriptFunction),
                                 ErrorCodes.JSInterpreterFailure);

    // The mapReduce command and $where are not limited by the query heap size limit and will
    // succeed even if it is set too low.
    setHeapSizeLimitMB({db: db, queryLimit: tooSmallHeapSizeMB, globalLimit: sufficentHeapSizeMB});
    assert.commandWorked(db.runCommand(mapReduce));
    // TODO SERVER-45454: Uncomment when $where is executed via  aggregation JavaScript expression.
    // assert.commandWorked(db.runCommand(findWithWhere));

    // All commands are expected to fail when the global heap size limit is too low, regardless
    // of the aggregation limit.
    setHeapSizeLimitMB({db: db, queryLimit: sufficentHeapSizeMB, globalLimit: tooSmallHeapSizeMB});
    assert.commandFailedWithCode(db.runCommand(findWithJavaScriptFunction),
                                 ErrorCodes.JSInterpreterFailure);
    // TODO SERVER-45454: Uncomment when $where is executed via  aggregation JavaScript expression.
    // assert.commandFailedWithCode(db.runCommand(findWithWhere), ErrorCodes.JSInterpreterFailure);
    assert.commandFailedWithCode(db.runCommand(mapReduce), ErrorCodes.JSInterpreterFailure);
}

// Test command invocations that can execute JavaScript on either mongos or mongod.
runCommonTests(mongosDB);

// Test command invocations that will only execute JavaScript on the shards.
const shardDB = st.shard0.getDB("test");
runCommonTests(shardDB);
runShardTests(shardDB);

st.stop();
}());
