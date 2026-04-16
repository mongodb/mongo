/**
 * Tests that wildcard indexes can be created successfully via index builds, including basic $**
 * indexes, compound wildcard indexes, wildcard indexes with projections, and wildcard indexes that
 * become multikey due to concurrent writes during the build.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const primaryDB = primary.getDB(dbName);

/**
 * Runs a single wildcard index build test case. Pauses the index build after setup (when the
 * side-write interceptor is already active), performs concurrent writes, then resumes and verifies
 * the index was created.
 */
function runWildcardBuildTest({testName, indexSpec, indexOptions = {}, seedDocs = [], concurrentDocs = []}) {
    jsTestLog(`---- ${testName} ----`);

    const collName = testName;
    const coll = primaryDB.getCollection(collName);
    coll.drop();
    assert.commandWorked(primaryDB.createCollection(collName));

    if (seedDocs.length > 0) {
        assert.commandWorked(coll.insertMany(seedDocs));
    }
    rst.awaitReplication();

    const indexName = "wildcard_idx";
    const options = Object.assign({name: indexName}, indexOptions);

    // Pause the index build after setup. At this point the side-write interceptor is active,
    // so any writes during the pause will be captured as side-writes.
    IndexBuildTest.pauseIndexBuilds(primary);

    const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), indexSpec, options);
    IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, indexName);

    IndexBuildTest.assertIndexesSoon(coll, 2, ["_id_"], [indexName]);

    for (const doc of concurrentDocs) {
        assert.commandWorked(coll.insert(doc));
    }

    IndexBuildTest.resumeIndexBuilds(primary);
    awaitIndex();

    IndexBuildTest.assertIndexes(coll, 2, ["_id_", indexName]);
}

// ---------------------------------------------------------------------------
// Test 1: Basic all-paths wildcard index with no concurrent writes.
// ---------------------------------------------------------------------------
runWildcardBuildTest({
    testName: "basic_wildcard",
    indexSpec: {"$**": 1},
    seedDocs: [
        {a: 1, b: "hello"},
        {a: 2, b: "world", c: {nested: true}},
        {x: 100, y: [1, 2, 3]},
    ],
});

// ---------------------------------------------------------------------------
// Test 2: Wildcard index with concurrent writes that make it multikey.
// ---------------------------------------------------------------------------
runWildcardBuildTest({
    testName: "wildcard_concurrent_multikey",
    indexSpec: {"$**": 1},
    seedDocs: [{a: 1}, {a: 2}, {a: 3}],
    concurrentDocs: [
        {a: [10, 20, 30]}, // Array makes the index multikey on 'a'.
        {b: {c: [4, 5]}}, // Nested array.
        {d: "new_field_value"}, // New field path.
    ],
});

// ---------------------------------------------------------------------------
// Test 3: Subtree-path compound wildcard index (e.g., {a: 1, "b.$**": 1}).
// ---------------------------------------------------------------------------
runWildcardBuildTest({
    testName: "compound_wildcard_subtree",
    indexSpec: {a: 1, "b.$**": 1},
    seedDocs: [
        {a: 1, b: {x: 10, y: 20}},
        {a: 2, b: {z: 30}},
        {a: 3, b: {x: 40, y: 50, z: 60}},
    ],
    concurrentDocs: [
        {a: 4, b: {w: 70}},
        {a: 5, b: {x: [80, 90]}}, // Multikey under b.
    ],
});

// ---------------------------------------------------------------------------
// Test 4: Compound wildcard index with wildcardProjection excluding the
// non-wildcard field (required for $** compound indexes).
// ---------------------------------------------------------------------------
runWildcardBuildTest({
    testName: "compound_wildcard_proj",
    indexSpec: {"$**": 1, a: 1},
    indexOptions: {wildcardProjection: {a: 0}},
    seedDocs: [
        {a: 1, b: 10, c: 20, d: 30},
        {a: 2, b: 40, c: 50, d: 60},
    ],
    concurrentDocs: [{a: 3, b: 70, c: 80, d: 90}],
});

// ---------------------------------------------------------------------------
// Test 5: Compound wildcard index with multiple non-wildcard fields,
// all excluded from the wildcardProjection.
// ---------------------------------------------------------------------------
runWildcardBuildTest({
    testName: "compound_wildcard_multi_excl",
    indexSpec: {a: 1, "$**": -1, b: 1},
    indexOptions: {wildcardProjection: {a: 0, b: 0}},
    seedDocs: [
        {a: 1, b: 10, c: "s1", d: 100},
        {a: 2, b: 20, c: "s2", d: 200},
    ],
    concurrentDocs: [{a: 3, b: 30, c: "s3", d: 300, e: "extra"}],
});

// ---------------------------------------------------------------------------
// Test 6: Wildcard index with deeply nested documents.
// ---------------------------------------------------------------------------
runWildcardBuildTest({
    testName: "wildcard_deeply_nested",
    indexSpec: {"$**": 1},
    seedDocs: [{a: {b: {c: {d: {e: 1}}}}}, {a: {b: {c: {d: {e: 2, f: "hello"}}}}}],
    concurrentDocs: [
        {a: {b: {c: {d: {e: [3, 4]}}}}}, // Nested multikey.
        {a: {b: {g: "new_path"}}},
    ],
});

// ---------------------------------------------------------------------------
// Test 7: Wildcard index with most data arriving as concurrent side-writes.
// ---------------------------------------------------------------------------
runWildcardBuildTest({
    testName: "wildcard_mostly_sidewrites",
    indexSpec: {"$**": 1},
    seedDocs: [{z: "seed"}],
    concurrentDocs: [{a: 1, b: "x"}, {c: [1, 2, 3]}, {d: {e: {f: 100}}}],
});

// ---------------------------------------------------------------------------
// Test 8: Wildcard index with updates and deletes as concurrent writes.
// ---------------------------------------------------------------------------
{
    jsTestLog("---- wildcard_concurrent_updates_deletes ----");

    const collName = "wildcard_concurrent_updates_deletes";
    const coll = primaryDB.getCollection(collName);
    coll.drop();
    assert.commandWorked(primaryDB.createCollection(collName));

    assert.commandWorked(
        coll.insertMany([
            {_id: 1, a: 10, b: "original"},
            {_id: 2, a: 20, b: "original"},
            {_id: 3, a: 30, b: "original"},
            {_id: 4, a: 40, b: "to_delete"},
        ]),
    );
    rst.awaitReplication();

    const indexName = "wildcard_idx";
    IndexBuildTest.pauseIndexBuilds(primary);
    const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {"$**": 1}, {name: indexName});
    IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, indexName);

    // Concurrent updates and deletes while the build is paused.
    assert.commandWorked(coll.updateOne({_id: 1}, {$set: {a: [100, 200]}})); // Becomes multikey.
    assert.commandWorked(coll.updateOne({_id: 2}, {$set: {c: "new_field"}}));
    assert.commandWorked(coll.deleteOne({_id: 4}));
    assert.commandWorked(coll.insert({_id: 5, d: {nested: [1, 2]}}));

    IndexBuildTest.resumeIndexBuilds(primary);
    awaitIndex();

    IndexBuildTest.assertIndexes(coll, 2, ["_id_", indexName]);

    // Verify the updated document is queryable via the wildcard index.
    const result = coll.find({a: 100}).hint(indexName).toArray();
    assert.eq(1, result.length, "Expected to find updated document via wildcard index");
    assert.eq(1, result[0]._id);

    // Verify the deleted document is not returned.
    assert.eq(0, coll.find({b: "to_delete"}).hint(indexName).itcount());
}

// ---------------------------------------------------------------------------
// Test 9: Multiple wildcard indexes built sequentially on the same collection.
// ---------------------------------------------------------------------------
{
    jsTestLog("---- multiple_wildcard_builds ----");

    const collName = "multiple_wildcard_builds";
    const coll = primaryDB.getCollection(collName);
    coll.drop();
    assert.commandWorked(primaryDB.createCollection(collName));

    assert.commandWorked(
        coll.insertMany([
            {a: 1, b: {x: 10}, c: "hello"},
            {a: 2, b: {y: 20}, c: "world"},
            {a: 3, b: {z: 30}, c: "test"},
        ]),
    );
    rst.awaitReplication();

    assert.commandWorked(coll.createIndex({"$**": 1}, {name: "allpaths_idx"}));
    assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1}, {name: "compound_wc_idx"}));

    IndexBuildTest.assertIndexes(coll, 3, ["_id_", "allpaths_idx", "compound_wc_idx"]);
}

// ---------------------------------------------------------------------------
// Test 10: Wildcard index build abort via dropIndexes.
// ---------------------------------------------------------------------------
{
    jsTestLog("---- wildcard_abort_via_dropIndexes ----");

    const collName = "wildcard_abort_via_dropIndexes";
    const coll = primaryDB.getCollection(collName);
    coll.drop();
    assert.commandWorked(primaryDB.createCollection(collName));

    assert.commandWorked(
        coll.insertMany([
            {a: 1, b: 2},
            {a: 3, b: 4},
        ]),
    );
    rst.awaitReplication();

    const indexName = "wildcard_to_abort";
    IndexBuildTest.pauseIndexBuilds(primary);

    const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {"$**": 1}, {name: indexName}, [
        ErrorCodes.IndexBuildAborted,
    ]);
    IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, indexName);

    // Abort the in-progress wildcard index build via dropIndexes.
    assert.commandWorked(primaryDB.runCommand({dropIndexes: collName, index: indexName}));

    IndexBuildTest.resumeIndexBuilds(primary);
    awaitIndex();

    // Only the _id index should remain.
    IndexBuildTest.assertIndexes(coll, 1, ["_id_"]);
}

rst.stopSet();
