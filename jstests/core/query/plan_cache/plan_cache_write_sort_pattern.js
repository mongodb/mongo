/**
 * Verifies that the sort pattern of a write command planned with a SORT_MERGE stage is reported
 * correctly by $planCacheStats after the command has completed.
 *
 * The plan cache retains the candidate plans' stats in its debug info, including
 * 'MergeSortStats::sortPattern', and serializes them on demand long after the originating request
 * is gone. The sort pattern therefore has to be an owned BSONObj rather than a view into the
 * request buffer. Covers both commands that accept a per-statement sort: update and bulkWrite.
 *
 * Under a sanitizer this detects a stale view directly. Without one, the comparison against the
 * requested sort pattern only catches the case where the underlying memory has been reused.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   # updateOne gained 'sort' in 8.0. Not redundant with multiversion_incompatible below: the FCV
 *   # upgrade/downgrade passthroughs are single-binary but vary FCV.
 *   requires_fcv_80,
 *   # Older binaries still have the use-after-free this test detects, so running against them
 *   # would be an intermittent failure rather than a regression signal.
 *   multiversion_incompatible,
 *   # Since the plan cache is per-node state, this test assumes that all operations are happening
 *   # against the same mongod.
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   does_not_support_stepdowns,
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 *   # An implicitly created index would change which plans the multi-planner considers.
 *   assumes_no_implicit_index_creation,
 *   requires_getmore,
 * ]
 */
import {getPlanStages} from "jstests/libs/analyze_plan.js";

const kSortPattern = {c: 1};

// The $or is wrapped in an $and with an extra predicate so the planner does not decompose the query
// with a SUBPLAN stage. SUBPLAN plans each $or branch independently and never populates the plan
// cache, which the assertions below depend on.
const kFilter = {$and: [{$or: [{a: 1}, {b: 2}]}, {c: {$gte: 0}}]};

/**
 * Creates a collection whose indexes make a SORT_MERGE plan available for 'kFilter' sorted by
 * 'kSortPattern', and which also admits a competing plan so that the query goes through the
 * multi-planner and produces a plan cache entry.
 */
function setUpCollection(name) {
    const coll = db.getCollection(name);
    coll.drop();

    const docs = [];
    for (let i = 0; i < 200; ++i) {
        docs.push({a: i % 10, b: i % 10, c: i});
    }
    assert.commandWorked(coll.insert(docs));

    // {a: 1, c: 1} and {b: 1, c: 1} let each $or branch be answered by an index while preserving the
    // {c: 1} sort order, which is what makes SORT_MERGE available. {c: 1} supplies the competing
    // plan.
    assert.commandWorked(coll.createIndex({a: 1, c: 1}));
    assert.commandWorked(coll.createIndex({b: 1, c: 1}));
    assert.commandWorked(coll.createIndex({c: 1}));

    return coll;
}

/**
 * Asserts that the collection has a plan cache entry containing at least one SORT_MERGE stage, and
 * that every such stage reports 'kSortPattern'. The SORT_MERGE plan is usually the rejected
 * candidate rather than the winner, so the cached plan and all candidate plans are searched.
 */
function assertCachedSortPatternIsCorrect(coll) {
    const entries = coll.aggregate([{$planCacheStats: {}}]).toArray();
    assert.gt(entries.length, 0, "write did not create a plan cache entry", {entries});

    const roots = entries.flatMap((entry) => [
        entry.cachedPlan,
        ...(entry.creationExecStats ?? []).map((stats) => stats.executionStages),
    ]);
    const sortMergeStages = roots
        .filter((root) => root)
        .flatMap((root) => getPlanStages(root, "SORT_MERGE"));

    assert.gt(sortMergeStages.length, 0, "no SORT_MERGE stage in the plan cache", {entries});
    for (const stage of sortMergeStages) {
        assert.eq(stage.sortPattern, kSortPattern, "unexpected cached sort pattern", {stage});
    }
}

// Updates are always planned by the classic engine, so the expected plan shape holds under any
// value of internalQueryFrameworkControl.
(function updateCommand() {
    const coll = setUpCollection(jsTestName() + "_update");

    assert.commandWorked(
        db.runCommand({
            update: coll.getName(),
            updates: [{q: kFilter, u: {$set: {x: 1}}, sort: kSortPattern, multi: false}],
        }),
    );

    assertCachedSortPatternIsCorrect(coll);
    coll.drop();
})();

(function bulkWriteCommand() {
    const coll = setUpCollection(jsTestName() + "_bulk_write");

    const res = assert.commandWorked(
        db.adminCommand({
            bulkWrite: 1,
            ops: [
                {
                    update: 0,
                    filter: kFilter,
                    updateMods: {$set: {x: 1}},
                    sort: kSortPattern,
                    multi: false,
                },
            ],
            nsInfo: [{ns: coll.getFullName()}],
        }),
    );
    assert.eq(res.nErrors, 0, "bulkWrite reported errors", {res});

    assertCachedSortPatternIsCorrect(coll);
    coll.drop();
})();
