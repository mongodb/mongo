/**
 * WASM and Native MozJS have different memory tracking mechanisms causing this split.
 * @tags: [
 *   requires_scripting,
 *   # This test has different behavior between the WAsm and Native MozJS engine.
 *   requires_fcv_90,
 *   # TODO SERVER-128404: Wasmtime OOM trap signal handler allocates memory; TSan aborts.
 *   tsan_incompatible,
 *   # ShardingTest with WASM mongod nodes is memory-intensive: each WASM mongod reserves
 *   # ~1.2 GB virtual memory per context pool entry. Serialize in Evergreen to avoid OOM.
 *   resource_intensive,
 * ]
 */

// Confirms that JavaScript heap limits are respected in aggregation. Includes testing for mapReduce
// and $where which use aggregation for execution.
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isMozjsWasm} from "jstests/libs/js_engine_util.js";

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
if (!isMozjsWasm(mongosDB)) {
    jsTestLog("Skipping: test requires mozjs-wasm engine");
    st.stop();
    quit();
}

// Shard the collection with one chunk per shard. Insert a single document into each shard.
st.shardColl(mongosColl.getName(), {x: 1}, {x: 2}, {x: 2});
assert.commandWorked(mongosColl.insert([{x: 0}, {x: 2}]));

// In the WASM JS engine, `internalQueryJavaScriptHeapSizeLimitMB` maps to SpiderMonkey's
// JSGC_MAX_BYTES, which is a GC-frequency hint rather than a hard allocation barrier.
// SpiderMonkey can grow past this limit by requesting more WASM linear memory (memory.grow),
// so the real hard limit is the Wasmtime store ceiling (wasmtimeStoreMemoryLimitMB).
// We set the store limit proportionally to the effective JS heap limit so the store becomes
// the binding OOM constraint.  The minimum store for a 50 MB JS heap is
//   50 + max(64, 50/10) = 114 MB
// which is tight enough to OOM with the 2M-object allocation in allocateLargeObject().
// The default store limit (1210 MB) is used for the "sufficient" case to guarantee success.
const tooSmallHeapSizeMB = 50;
// mapReduce's emit buffer requires floor(internalQueryMaxJsEmitBytes + BSONObjMaxInternalSize)
// = 100 MB + 16 MB = 116 MB of WASM linear memory. 117 MB passes this size check (1 MB headroom)
// while remaining below allocateLargeObject()'s actual OOM footprint on all CI platforms:
// ~120 MB on ARM64 and ~170 MB on x86_64. Do NOT raise this above 120 MB or the test will not
// OOM on ARM64 machines.
const tooSmallStoreSizeMB = 117;
const sufficentHeapSizeMB = 200;
const sufficentStoreSizeMB = 1210; // default Wasmtime store; well above the 2M-object footprint

// When the Wasmtime store limiter denies memory.grow, SpiderMonkey MOZ_CRASHes via an
// `unreachable` trap, which the WASM bridge classifies as ExceededMemoryLimit.
// JSInterpreterFailure cannot occur here: in the WASM engine, JS errors are returned through
// the WIT result type, not as traps, so a memory.grow denial never surfaces as
// JSInterpreterFailure.
const expectedOOMErrorCodes = [ErrorCodes.ExceededMemoryLimit];

function setHeapSizeLimitMBOneNode({db, queryLimit, globalLimit, storeLimit}) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryJavaScriptHeapSizeLimitMB: queryLimit}),
    );
    assert.commandWorked(db.adminCommand({setParameter: 1, jsHeapLimitMB: globalLimit}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, wasmtimeStoreMemoryLimitMB: storeLimit}),
    );
}

function setHeapSizeLimitMB({queryLimit, globalLimit, storeLimit}) {
    st.forEachMongos((conn) => {
        setHeapSizeLimitMBOneNode({
            db: conn.getDB("test"),
            queryLimit: queryLimit,
            globalLimit: globalLimit,
            storeLimit: storeLimit,
        });
    });
    st.forEachConnection((conn) => {
        setHeapSizeLimitMBOneNode({
            db: conn.getDB("test"),
            queryLimit: queryLimit,
            globalLimit: globalLimit,
            storeLimit: storeLimit,
        });
    });
}

// Number of objects needed to push WASM linear memory above tooSmallStoreSizeMB.
// Each object occupies one 64-byte GC arena slot; tooSmallStoreSizeMB / 64 B gives the
// minimum to fill that many MB in the GC heap alone, with a ~5% margin for reliability.
const kAllocObjectCount = Math.ceil(((tooSmallStoreSizeMB * 1024 * 1024) / 64) * 1.05);

// Allocate kAllocObjectCount live JS objects to push WASM linear memory past tooSmallStoreSizeMB.
// Each element is ~64 bytes in the GC arena plus SpiderMonkey runtime overhead, so the total WASM
// linear memory footprint exceeds the tight store limit but is well below sufficentStoreSizeMB.
//
// Defined with new Function() so kAllocObjectCount is baked in as a numeric literal when the
// function is serialized for server-side evaluation inside mapReduce and aggregation operators.
const allocateLargeObject = new Function(`
    var arr = [];
    for (var i = 0; i < ${kAllocObjectCount}; i++) {
        arr.push({x: i, y: i * 2, z: "padding_string_to_consume_memory"});
    }
    return true;
`);

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
    setHeapSizeLimitMB({
        queryLimit: sufficentHeapSizeMB,
        globalLimit: sufficentHeapSizeMB,
        storeLimit: sufficentStoreSizeMB,
    });
    assert.commandWorked(db.runCommand(aggregateWithJSFunction));
    assert.commandWorked(db.runCommand(aggregateWithInternalJsReduce));
    assert.commandWorked(db.runCommand(aggregateWithUserDefinedAccumulator));

    // The aggregate command is expected to fail when the aggregation specific heap size limit is
    // too low.
    setHeapSizeLimitMB({
        queryLimit: tooSmallHeapSizeMB,
        globalLimit: sufficentHeapSizeMB,
        storeLimit: tooSmallStoreSizeMB,
    });
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
    setHeapSizeLimitMB({
        queryLimit: sufficentHeapSizeMB,
        globalLimit: tooSmallHeapSizeMB,
        storeLimit: tooSmallStoreSizeMB,
    });
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
    setHeapSizeLimitMB({
        queryLimit: sufficentHeapSizeMB,
        globalLimit: sufficentHeapSizeMB,
        storeLimit: sufficentStoreSizeMB,
    });
    assert.commandWorked(db.runCommand(findWithJavaScriptFunction));
    assert.commandWorked(db.runCommand(findWithWhere));
    assert.commandWorked(db.runCommand(mapReduce));

    // A find command with JavaScript agg expression is expected to fail when the query specific
    // heap size limit is too low.
    setHeapSizeLimitMB({
        queryLimit: tooSmallHeapSizeMB,
        globalLimit: sufficentHeapSizeMB,
        storeLimit: tooSmallStoreSizeMB,
    });
    assert.commandFailedWithCode(db.runCommand(findWithJavaScriptFunction), expectedOOMErrorCodes);

    // The mapReduce command and $where are not limited by the query heap size limit and will
    // succeed even if it is set too low.
    setHeapSizeLimitMB({
        queryLimit: tooSmallHeapSizeMB,
        globalLimit: sufficentHeapSizeMB,
        storeLimit: sufficentStoreSizeMB,
    });
    assert.commandWorked(db.runCommand(mapReduce));
    assert.commandWorked(db.runCommand(findWithWhere));

    // All commands are expected to fail when the global heap size limit is too low, regardless
    // of the aggregation limit.
    setHeapSizeLimitMB({
        queryLimit: sufficentHeapSizeMB,
        globalLimit: tooSmallHeapSizeMB,
        storeLimit: tooSmallStoreSizeMB,
    });
    assert.commandFailedWithCode(db.runCommand(findWithJavaScriptFunction), expectedOOMErrorCodes);
    assert.commandFailedWithCode(db.runCommand(findWithWhere), expectedOOMErrorCodes);
    assert.commandFailedWithCode(db.runCommand(mapReduce), expectedOOMErrorCodes);
}

// Test command invocations that can execute JavaScript on either mongos or mongod.
runCommonTests(mongosDB);

// Test command invocations that will only execute JavaScript on the shards.
const shardDB = st.shard0.getDB("test");
runCommonTests(shardDB);
runShardTests(shardDB);

st.stop();
