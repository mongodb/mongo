/**
 * The internal form of $search returns two cursors (results + metadata) under one operation. This
 * test kills the metadata cursor and drives getMore on the results cursor to confirm it's still
 * alive.
 *
 * This test exists to prove that usage of the OperationMemoryUsageTracker with this $search
 * scenario works. Because per-operation memory tracking is now disabled for this shape, the test
 * currently exercises running the query and killing the cursors in a particular order without
 * memory tracking. This test is kept to ensure this scenario works when memory tracking is enabled
 * for $search $$SEARCH_META queries.
 *
 * Since memory tracking should be disabled for this scenario, we confirm no memory tracking metrics exist.
 *
 * TODO SERVER-132669: Update the comment above to reflect that memory tracking is enabled for this
 * scenario.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   requires_profiling,
 *   requires_getmore,
 *   queries_system_profile_collection,
 * ]
 */
import {verifyProfilerMetrics} from "jstests/libs/query/memory_tracking_utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const testDB = db.getSiblingDB(jsTestName());
const collName = "c";
const coll = testDB.getCollection(collName);
const searchIndexName = "two_cursor_memtrack_index";

coll.drop();
const kNumDocs = 1000;
const kBatchSize = 200;
const seed = [];
for (let i = 0; i < kNumDocs; i++) {
    seed.push({_id: i, k: i, t: "hello"});
}
assert.commandWorked(coll.insert(seed));

createSearchIndex(coll, {name: searchIndexName, definition: {mappings: {dynamic: true}}});

// Authenticate as internal to run internal command.
const internalConn = new Mongo(db.getMongo().host);
jsTest.authenticate(internalConn);
assert.commandWorked(
    internalConn.adminCommand({
        hello: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
    }),
);
const idb = internalConn.getDB(testDB.getName());

const mongotQuery = {index: searchIndexName, text: {query: "hello", path: "t"}};

// We pretend to be a router sending the pipeline to a shard.
const searchStages = [
    {
        $search: {
            mongotQuery: mongotQuery,
            metadataMergeProtocolVersion: NumberInt(1),
            requiresSearchMetaCursor: true,
        },
    },
];

const comment = jsTestName();

// Get previous curOp memory usage so we can restore it after this test.
const prevMaxWriteToCurOpMemoryUsageBytes = assert.commandWorked(
    idb.adminCommand({getParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 1}),
).internalQueryMaxWriteToCurOpMemoryUsageBytes;

try {
    // Confirm memory tracking isn't occurring for this scenario.
    testDB.setProfilingLevel(2);

    // Flush tracked memory to CurOp on every update, so that any tracking that happens at all
    // reaches the profiler instead of staying buffered below the reporting threshold.
    assert.commandWorked(idb.adminCommand({setParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 1}));

    const res = assert.commandWorked(
        idb.runCommand({
            aggregate: collName,
            fromRouter: true,
            needsMerge: true,
            // Internal-client connections must send an explicit writeConcern.
            writeConcern: {w: 1},
            cursor: {batchSize: 0},
            comment: comment,
            pipeline: [
                ...searchStages,
                {
                    $setWindowFields: {
                        sortBy: {k: 1},
                        output: {s: {$sum: "$k", window: {documents: [-5, 5]}}},
                    },
                },
            ],
        }),
    );

    // The internal merge form yields the results + metadata two-cursor state.
    assert(res.cursors, "expected a multi-cursor reply", {res});
    assert.eq(res.cursors.length, 2, "expected results + meta cursors", {res});
    const main = res.cursors.find((c) => c.cursor.type !== "meta").cursor;
    const meta = res.cursors.find((c) => c.cursor.type === "meta").cursor;
    jsTest.log.info("two-cursor state", {mainId: main.id, metaId: meta.id});

    // Kill metadata cursor, which co-owns the OperationMemoryUsageTracker referenced by
    // $setWindowFields in the main cursor.
    assert.commandWorked(idb.runCommand({killCursors: collName, cursors: [meta.id]}));

    // getMore(main) drives $setWindowFields over the buffered results, exercising the operation
    // tracker after the sibling cursor is gone.
    let gm = assert.commandWorked(idb.runCommand({getMore: main.id, collection: collName, batchSize: kBatchSize}));
    while (gm.cursor.id.toNumber() !== 0) {
        gm = assert.commandWorked(idb.runCommand({getMore: gm.cursor.id, collection: collName, batchSize: kBatchSize}));
    }

    // The server must still be responsive after the sequence.
    assert.commandWorked(idb.adminCommand({ping: 1}), "mongod is no longer responsive");

    testDB.setProfilingLevel(0);
    const profilerEntries = testDB.system.profile.find({"command.comment": comment}).toArray();

    assert(
        profilerEntries.some((entry) => entry.op === "getmore"),
        "expected getMore entries among the profiled operations",
        {profilerEntries},
    );

    // Confirm no memory metrics appear.
    verifyProfilerMetrics({profilerEntries, verifyOptions: {expectMemoryMetrics: false}});
} finally {
    testDB.setProfilingLevel(0);
    testDB.system.profile.drop();
    assert.commandWorked(
        idb.adminCommand({
            setParameter: 1,
            internalQueryMaxWriteToCurOpMemoryUsageBytes: prevMaxWriteToCurOpMemoryUsageBytes,
        }),
    );
    dropSearchIndex(coll, {name: searchIndexName});
}
