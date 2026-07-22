/**
 * Test that different agg stages respect BSON size limits.
 *
 * @tags: [
 *     # Overflows WT cache on in-memory variants.
 *     requires_persistence,
 *     requires_getmore,
 *     # Inserts large documents into a single transaction, causing
 *     # TransactionTooLargeForCache when run under the multi-statement txn passthrough.
 *     does_not_support_transactions,
 *     # This test relies on query commands returning specific batch-sized responses.
 *     assumes_no_implicit_cursor_exhaustion,
 *     # Inserts documents that are too large for a timeseries collection.
 *     exclude_from_timeseries_crud_passthrough,
 *     # Inserts large documents, causing severe WiredTiger eviction pressure on the
 *     # initial sync node and making the primary unreachable.
 *     incompatible_with_initial_sync,
 *     # Creates memory pressure on secondaries in some passthroughs (esp. secondary_reads_passthrough)
 *     # under burn-in configuration.
 *     assumes_read_preference_unchanged,
 *     # Inserts large documents; must run in the resource_intensive bucket so it is
 *     # not run in parallel with other tests on memory-constrained machines.
 *     resource_intensive,
 * ]
 */

const collName = jsTestName();
const collExact = db[collName + "_exact"];
const collExactPlusOne = db[collName + "_exact_plus_one"];
const collMany = db[collName + "_many"];

collExact.drop();
collExactPlusOne.drop();
collMany.drop();

// The max size limit for a stored document is 16 MB.
const bsonMaxSizeLimit = 16 * 1024 * 1024;
// The internal pipeline limit. In-flight pipeline documents may reach this limit even though
// they cannot be stored.
const bsonInternalSizeLimit = bsonMaxSizeLimit + 16 * 1024;

// Insert a document that is exactly 16 MB.
const maxSizeDoc = {_id: 1, x: "a".repeat(bsonMaxSizeLimit - 26)};
assert.eq(Object.bsonsize(maxSizeDoc), bsonMaxSizeLimit);
assert.commandWorked(collExact.insert(maxSizeDoc));

// Insert two documents, one that is exactly 16 MB and other that has roughly 200 B.
assert.commandWorked(collExactPlusOne.insert(maxSizeDoc));
assert.commandWorked(collExactPlusOne.insert({_id: 2, y: "b".repeat(200)}));

const payloadSize = 20000;

// 1 000 documents at ~20 KB each. The sum total is ~20 MB, which
// is above BSONObjMaxInternalSize.
const nDocs = 1000;
assert.commandWorked(
    collMany.insertMany(
        Array.from({length: nDocs}, (_, i) => ({_id: i, data: "x".repeat(payloadSize)})),
    ),
);

/** Runs an aggregation on `coll`, asserts it returns a valid size document. */
function assertAggSucceedsWithUserRangeBSON(coll, pipeline, msg) {
    const res = coll.aggregate(pipeline).toArray();
    assert.gte(res.length, 1, msg);
    const sz = Object.bsonsize(res[0]);
    assert.lte(sz, bsonMaxSizeLimit, `${msg}: expected doc size <= 16 MB, got ${sz}`);
    return res;
}

/** Runs an aggregation on `coll` and asserts it fails with BSONObjectTooLarge. */
function assertAggFailsBSONTooLarge(coll, pipeline, msg) {
    assert.throwsWithCode(
        () => coll.aggregate(pipeline).toArray(),
        ErrorCodes.BSONObjectTooLarge,
        [],
        msg,
    );
}

/**
 * Runs an aggregation on `coll` and asserts the first result document's BSON size is strictly
 * between bsonMaxSizeLimit (16 MB) and bsonInternalSizeLimit (16 MB + 16 KB).
 */
function assertAggSucceedsWithInternalRangeBSON(coll, pipeline, msg) {
    const res = coll.aggregate(pipeline).toArray();
    assert.gte(res.length, 1, msg);
    const sz = Object.bsonsize(res[0]);
    assert.gt(sz, bsonMaxSizeLimit, `${msg}: expected doc size > 16 MB, got ${sz}`);
    assert.lt(sz, bsonInternalSizeLimit, `${msg}: expected doc size < 16 MB + 16 KB, got ${sz}`);
    return res;
}

function testGroupPush() {
    // $group + $push of collExactPlusOne, which pushes the BSON size slightly above 16 MB.
    assertAggSucceedsWithInternalRangeBSON(
        collExactPlusOne,
        [{$group: {_id: null, all: {$push: "$$ROOT"}}}],
        "$group + $push (last stage) with result just over 16 MB in internal range should succeed",
    );

    // ~20 MB result (collMany) exceeds BSONObjMaxInternalSize - fails.
    assertAggFailsBSONTooLarge(
        collMany,
        [{$group: {_id: null, all: {$push: "$$ROOT"}}}],
        "$group + $push (last stage) with ~20 MB result should fail with BSONObjectTooLarge",
    );
}

function testGroupNonLastStage() {
    // $group + $push of collExactPlusOne, which pushes the BSON size slightly above 16 MB.
    assertAggSucceedsWithInternalRangeBSON(
        collExactPlusOne,
        [{$group: {_id: null, all: {$push: "$$ROOT"}}}, {$limit: 1}],
        "$group + $push (non-last stage) result just over 16 MB in internal range should succeed",
    );

    // ~20 MB result (collMany) exceeds BSONObjMaxInternalSize - fails.
    assertAggFailsBSONTooLarge(
        collMany,
        [{$group: {_id: null, all: {$push: "$$ROOT"}}}, {$limit: 1}],
        "$group + $push (non-last stage) with ~20 MB result should fail with BSONObjectTooLarge",
    );

    // Two groups: "small" (_id: 0, 5 docs × 20 KB ≈ 100 KB) and "large" (_id: 1, 995 docs ×
    // 20 KB ≈ 20 MB). $limit: 1 returns only the small group.
    // If size validation ran intra-stage (at group construction time), the pipeline would fail
    // when the large group is built. Since validation is deferred to the return point only, the
    // pipeline succeeds.
    assertAggSucceedsWithUserRangeBSON(
        collMany,
        [
            {$group: {_id: {$cond: [{$lt: ["$_id", 5]}, 0, 1]}, all: {$push: "$data"}}},
            {$sort: {_id: 1}},
            {$limit: 1},
        ],
        "two groups (small + oversized): returning only the small group should succeed",
    );
}

function testAddFields() {
    // Adding a tiny field to the 16 MB doc produces a result just over 16 MB — in the
    // internal-only range, succeeds.
    assertAggSucceedsWithInternalRangeBSON(
        collExact,
        [{$addFields: {y: "a"}}],
        "$addFields adding a small field to a 16 MB doc in internal range should succeed",
    );

    // Duplicating the ~16 MB field produces a ~32 MB result — fails.
    assertAggFailsBSONTooLarge(
        collExact,
        [{$addFields: {b: "$x"}}],
        "$addFields duplicating a ~16 MB field should fail with BSONObjectTooLarge",
    );
}

function testProject() {
    // Projected doc < 16 MB — succeeds.
    const r = assertAggSucceedsWithUserRangeBSON(
        collExact,
        [{$project: {_id: 0, result: {$concat: [{$toString: "$_id"}, {$toString: "$_id"}]}}}],
        "$project + $concat: doc < 16 MB should succeed",
    );
    const sz = Object.bsonsize(r[0]);
    assert.lt(sz, bsonMaxSizeLimit, `expected projected doc < 16 MB, got ${sz}`);

    // Projected doc in the internal-only range (just over 16 MB) — succeeds.
    assertAggSucceedsWithInternalRangeBSON(
        collExact,
        [{$project: {_id: 0, result: "$x", second: "a".repeat(300)}}],
        "$project passing through a ~16 MB field with extra bytes in internal range should succeed",
    );

    // Duplicating the ~16 MB field into two output fields produces a ~32 MB doc — fails.
    assertAggFailsBSONTooLarge(
        collExact,
        [{$project: {_id: 0, result1: "$x", result2: "$x"}}],
        "$project duplicating a ~16 MB field into two output fields should fail with BSONObjectTooLarge",
    );
}

// Run all scenarios
testGroupPush();
testGroupNonLastStage();
testAddFields();
testProject();
