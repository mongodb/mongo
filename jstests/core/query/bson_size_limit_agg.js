/**
 * Test that different agg stages respect BSON size limits.
 * Also verifies that SBE's trial-run stash rejects oversized results.
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

import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {it} from "jstests/libs/mochalite.js";

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
function assertAggSucceedsWithUserRangeBSON(coll, pipeline) {
    const res = coll.aggregate(pipeline).toArray();
    assert.gte(res.length, 1, `expected the query to return at least one document`);
    const sz = Object.bsonsize(res[0]);
    assert.lte(sz, bsonMaxSizeLimit, `expected doc size <= 16 MB`);
    return res;
}

/** Runs an aggregation on `coll` and asserts it fails with BSONObjectTooLarge. */
function assertAggFailsBSONTooLarge(coll, pipeline, msg = "") {
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
    assert.gt(sz, bsonMaxSizeLimit, `${msg}: expected doc size > 16 MB`);
    assert.lt(sz, bsonInternalSizeLimit, `${msg}: expected doc size < 16 MB + 16 KB`);
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

function testGroupPushNonLastStage() {
    // Size limit: does not trigger.
    //
    // $group + $push that produces a document slightly above 16 MB, but still under the internal limit.
    assertAggSucceedsWithInternalRangeBSON(
        collExactPlusOne,
        [{$group: {_id: null, all: {$push: "$$ROOT"}}}, {$limit: 1}],
        "$group + $push (non-last stage) result just over 16 MB in internal range should succeed",
    );

    // Size limit: triggers.
    //
    // $group + $push that produces a document of approximately 20 MB.
    assertAggFailsBSONTooLarge(
        collMany,
        [{$group: {_id: null, all: {$push: "$$ROOT"}}}, {$limit: 1}],
        "$group + $push (non-last stage) with ~20 MB result should fail with BSONObjectTooLarge",
    );

    // Size limit: does not trigger.
    //
    // $group + $push resulting in two documents, the first of 100 KB and the second of 20 MB.
    // $sort + $limit will then exclude the second and return just the first, which is under the
    // size limit.
    assertAggSucceedsWithUserRangeBSON(collMany, [
        {$group: {_id: {$cond: [{$lt: ["$_id", 5]}, 0, 1]}, all: {$push: "$data"}}},
        {$sort: {_id: 1}},
        {$limit: 1},
    ]);
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
    const r = assertAggSucceedsWithUserRangeBSON(collExact, [
        {$project: {_id: 0, result: {$concat: [{$toString: "$_id"}, {$toString: "$_id"}]}}},
    ]);
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

function testSbeToClassicSplit() {
    // This query must work in all variants. But we're particularly interested in the
    // case where the first $project is pushed down to SBE and the second $project stays
    // in classic, where we expect SBE be able to return an overlarge document to the
    // classic portion of the query instead of checking the size limit.
    const pipeline = [
        {$project: {b: "$x"}},
        {$_internalInhibitOptimization: {}},
        {$project: {_id: 1}},
    ];
    assertAggSucceedsWithUserRangeBSON(collExact, pipeline);
    const explain = collExact.explain().aggregate(pipeline);
    if (checkSbeFullyEnabled(db)) {
        // First $project is in SBE, and the second one should run in classic.
        const stages = getAggPlanStages(explain, "$project");
        assert(stages[0].$project.hasOwnProperty("_id"));
        assert.eq(stages.length, 1);
    }
}

function testTrialRun() {
    const coll = db[collName + "_trial_run"];
    coll.drop();

    // Prepare the collection: a small document + two indexes, to trigger multi-planning.
    assert.commandWorked(coll.insertOne({_id: 1, a: 1, b: 1}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));

    // Run a query that adds a 200 KB field to the document.
    const pipeline = [{$match: {a: 1, b: 1}}, {$addFields: {extra: "x".repeat(200 * 1024)}}];

    // Activate cache entry.
    coll.aggregate(pipeline).toArray();
    coll.aggregate(pipeline).toArray();
    coll.aggregate(pipeline).toArray();

    // Replace with a near-limit document.
    const bsonUserLimit = 16 * 1024 * 1024;
    const paddingSize = bsonUserLimit - 4000;
    assert.commandWorked(
        coll.replaceOne({_id: 1}, {_id: 1, a: 1, b: 1, padding: "y".repeat(paddingSize)}),
    );

    // Hit the active cache entry and execute the trial run. Under SBE, this will
    // read some documents and stash them, which triggers a different branch for
    // returning documents to the user. This branch must do its own validation.
    assertAggFailsBSONTooLarge(coll, pipeline);
}

function testConvertIntermediaryDocument() {
    // Size limit: does not trigger. A $convert should be able to turn a document of
    // exactly max user size into BinData, which is then passed over to a hash function.
    // Before SERVER-135206, BinData would throw without allowing this valid intermediary
    // document.
    assertAggSucceedsWithUserRangeBSON(collExact, [
        {
            $project: {
                _id: 0,
                hash: {
                    $hash: {
                        algorithm: "xxh64",
                        input: {$convert: {input: "$$ROOT", to: "binData"}},
                    },
                },
            },
        },
    ]);
}

it("$group + $push", testGroupPush);
it("$group + $push (non-last stage)", testGroupPushNonLastStage);
it("$addFields", testAddFields);
it("$project", testProject);
it("SBE to classic split", testSbeToClassicSplit);
it("Plan cache trial run", testTrialRun);
it("$convert (non-last stage)", testConvertIntermediaryDocument);
