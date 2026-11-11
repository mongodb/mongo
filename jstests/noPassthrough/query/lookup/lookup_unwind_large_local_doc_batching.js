/**
 * Tests that the $cursor stage does not produce single-document batches when SBE $lookup+$unwind
 * results all share one large local document. Every result row is backed by the same BSON
 * buffer, so it should be counted toward the batch byte limit only once.
 *
 * @tags: [
 *     requires_fcv_90,
 *     requires_sbe,
 * ]
 */

import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";

function assertCursorBatchSize(db) {
    const localColl = db.local;
    const foreignColl = db.foreign;

    const nForeign = 200;
    const foreignDocs = [];
    for (let i = 0; i < nForeign; i++) {
        foreignDocs.push({_id: i, key: 1, b: i});
    }
    assert.commandWorked(foreignColl.insert(foreignDocs));

    // Around 5MB, larger than the default 4MB $cursor batch limit.
    const blob = Array.from({length: 250000}, () => "abcdefgh");
    assert.commandWorked(localColl.insert({_id: 0, key: 1, a: 1, blob}));

    const pipeline = [
        {$lookup: {from: "foreign", as: "joined", localField: "key", foreignField: "key"}},
        {$unwind: {path: "$joined"}},
        {$addFields: {computedValue: {$add: ["$a", "$joined.b"]}}},
        {$sort: {computedValue: 1}},
        {$limit: 1},
        {$project: {blob: 0}},
    ];

    const explain = localColl.explain("executionStats").aggregate(pipeline);
    assert.eq(getEngine(explain), "sbe");

    const scanStages = getAggPlanStages(explain, "scan");
    // Both local and foreign will use a plain scan.
    assert.eq(scanStages.length, 2, {explain});
    for (const scan of scanStages) {
        assert.lt(scan.restoreState, 50, "expected few $cursor batches", {scan});
    }

    assert.eq(localColl.aggregate(pipeline).toArray(), [
        {_id: 0, key: 1, a: 1, joined: {_id: 0, key: 1, b: 0}, computedValue: 1},
    ]);
}

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagSbeEqLookupUnwindHashJoin: true,
        featureFlagSbeEqLookupUnwindLocalCollscan: true,
        featureFlagSbeEfficientBoundaryMaterialization: true,
    },
});
assert.neq(null, conn, "mongod was unable to start up");
try {
    assertCursorBatchSize(conn.getDB(jsTestName()));
} finally {
    MongoRunner.stopMongod(conn);
}
