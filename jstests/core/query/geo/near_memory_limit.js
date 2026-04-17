/**
 * Test that verifies the behavior of NearStage when memory limit is set.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   assumes_unsharded_collection,
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_90,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   # setParameter may return different values after a failover.
 *   does_not_support_stepdowns,
 *   # $geoNear requires the 'key' option on timeseries collections.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = db.near_memory_limit;
coll.drop();

const kDocCount = 10000;
const docs = Array.from({length: kDocCount}, (_, i) => {
    // Spread documents across a range of coordinates to produce many distinct distances.
    const lng = (i % 360) - 180;
    const lat = (Math.floor(i / 360) % 180) - 90;
    return {_id: i, loc: {type: "Point", coordinates: [lng, lat]}};
});
assert.commandWorked(coll.insertMany(docs));
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

const pipeline = [{$geoNear: {near: {type: "Point", coordinates: [0, 0]}, distanceField: "dist"}}];

const explainRes = coll.explain("executionStats").aggregate(pipeline);
const nearStages = getAggPlanStages(explainRes, "GEO_NEAR_2DSPHERE");
if (nearStages.length === 0) {
    jsTest.log.info(
        "Skipping test: GEO_NEAR_2DSPHERE stage not found in explain. " +
            "This stage is only used by the classic engine.",
    );
    quit();
}

// The query should succeed with the default memory limit.
assert.gte(coll.aggregate(pipeline).itcount(), 0);

// Pick a memory limit that:
//   - is below the full peak (to trigger spilling of the result buffer), and
//   - is above _seenDocuments alone (so the query succeeds after spilling).
// We measure the actual peak from explain and use half of it, which works for any RecordId
// type (integer for regular collections, OID for clustered collections).
const peakMem = nearStages[0].peakTrackedMemBytes;
assert.gt(peakMem, 0, "Expected peakTrackedMemBytes in GEO_NEAR_2DSPHERE stage: " + tojson(explainRes));
const spillingLimit = Math.floor(peakMem / 2);

const originalNearStageMemory = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalNearStageMaxMemoryBytes: 1}),
);

const isSpillingEnabled = FeatureFlagUtil.isEnabled(db, "ExtendedAutoSpilling");

// A small limit forces the memory check to fire once enough documents are buffered.
try {
    setParameterOnAllNonConfigNodes(db.getMongo(), "internalNearStageMaxMemoryBytes", spillingLimit);

    if (isSpillingEnabled) {
        // The result buffer is spilled to disk. After each spill, only _seenDocuments remains
        // in memory. Since spillingLimit > _seenDocuments, the query succeeds.
        assert.eq(kDocCount, coll.aggregate(pipeline).itcount());
    } else {
        assert.commandFailedWithCode(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), [
            12227900,
        ]);
    }
} finally {
    setParameterOnAllNonConfigNodes(
        db.getMongo(),
        "internalNearStageMaxMemoryBytes",
        originalNearStageMemory.internalNearStageMaxMemoryBytes,
    );
}

if (isSpillingEnabled) {
    // With an extremely low limit, even after spilling the result buffer, _seenDocuments
    // alone (which cannot be spilled) exceeds the limit and the query fails.
    try {
        setParameterOnAllNonConfigNodes(db.getMongo(), "internalNearStageMaxMemoryBytes", 1000);
        assert.commandFailedWithCode(db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), [
            12227900,
        ]);
    } finally {
        setParameterOnAllNonConfigNodes(
            db.getMongo(),
            "internalNearStageMaxMemoryBytes",
            originalNearStageMemory.internalNearStageMaxMemoryBytes,
        );
    }
}
