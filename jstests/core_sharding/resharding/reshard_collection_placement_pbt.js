/**
 * Property-based test for reshardCollection's placement handling (zones / shardDistribution).
 *
 * @tags: [
 *  uses_atclustertime,
 *  assumes_unsharded_collection,
 *  cannot_run_during_upgrade_downgrade,
 *  # This test asserts specific chunk placements.
 *  assumes_balancer_off,
 *  # Stepdown test coverage is already provided by the resharding FSM suites.
 *  does_not_support_stepdowns,
 *  resource_intensive,
 * ]
 */
import {after, describe, it} from "jstests/libs/mochalite.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {getShardNames} from "jstests/sharding/libs/sharding_util.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const dbName = db.getName();
const collName = jsTestName();
const ns = dbName + "." + collName;
const testDB = db.getSiblingDB(dbName);
const configDB = db.getSiblingDB("config");
const mongos = db.getMongo();

// This test requires at least two shards.
const shardNames = getShardNames(db);
if (shardNames.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}
// Only the first two shards are used, so the model stays stable even in suites configured with
// more.
const [shardA, shardB] = shardNames;

const kZoneName = "reshardPlacementPbtZone";
const kNumInitialDocs = 50;

function keyPatternFor(keyShape) {
    switch (keyShape) {
        case "range":
            return {newKey: 1};
        case "hashedPrefix":
            return {newKey: "hashed"};
        case "hashedNonPrefix":
            return {oldKey: 1, newKey: "hashed"};
        case "compoundRange":
            return {newKey: 1, oldKey: 1};
    }
}

function isHashedPrefix(keyPattern) {
    return Object.values(keyPattern)[0] === "hashed";
}

const keyShapeArbitrary = fc.constantFrom(
    "range",
    "hashedPrefix",
    "hashedNonPrefix",
    "compoundRange",
);

// Each scenario is a distinct combination of {zones, shardDistribution} that maps to a distinct
// branch of calculateParticipantShardsAndChunks:
//  - "none": no placement requested -> hashed-prefix keys take the zone-unaware fast path.
//  - "zonesOnly": zones alone -> SamplingBasedSplitPolicy (live aggregation dispatch).
//  - "shardDistributionMinMax": shardDistribution with min/max -> ShardDistributionSplitPolicy.
//  - "shardDistributionShardListOnly": shardDistribution without min/max ->
//    SamplingBasedSplitPolicy restricted to the listed shards (live aggregation dispatch).
//  - "zonesAndShardDistributionMinMax": both supplied and agreeing.
const placementScenarios = [
    {placement: "none", targetShards: []},
    {placement: "zonesOnly", targetShards: [shardB]},
    {placement: "shardDistributionMinMax", targetShards: [shardB]},
    {placement: "shardDistributionShardListOnly", targetShards: [shardB]},
    {placement: "shardDistributionShardListOnly", targetShards: [shardA, shardB]},
    {placement: "zonesAndShardDistributionMinMax", targetShards: [shardB]},
];

const modelArbitrary = fc.record({
    keyShape: keyShapeArbitrary,
    scenario: fc.constantFrom(...placementScenarios),
});

function resetZoneAssignment() {
    for (const shard of shardNames) {
        // removeShardFromZone succeeds even if 'shard' isn't currently tagged with this zone
        // (it's a $pull, a no-op removal), so a failure here is a real problem -- e.g. the zone
        // still being associated with a chunk range from a prior iteration -- not an expected,
        // ignorable case.
        assert.commandWorked(mongos.adminCommand({removeShardFromZone: shard, zone: kZoneName}));
    }
}

function setUpCollection() {
    testDB[collName].drop();
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));
    const bulk = testDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < kNumInitialDocs; i++) {
        bulk.insert({oldKey: i, newKey: i});
    }
    assert.commandWorked(bulk.execute());
}

// Builds the {zones, shardDistribution} request fields for 'placement'/'targetShards' against
// 'keyPattern', assigning zone tags as a side effect. Returns the set of shards the request
// restricts placement to (empty if unrestricted).
function buildPlacementRequest(placement, targetShards, keyPattern) {
    resetZoneAssignment();

    const globalMin = {};
    const globalMax = {};
    for (const field of Object.keys(keyPattern)) {
        globalMin[field] = MinKey;
        globalMax[field] = MaxKey;
    }

    const request = {};
    if (placement === "zonesOnly" || placement === "zonesAndShardDistributionMinMax") {
        assert.commandWorked(
            mongos.adminCommand({addShardToZone: targetShards[0], zone: kZoneName}),
        );
        request.zones = [{zone: kZoneName, min: globalMin, max: globalMax}];
    }
    if (
        placement === "shardDistributionMinMax" ||
        placement === "zonesAndShardDistributionMinMax"
    ) {
        request.shardDistribution = [{shard: targetShards[0], min: globalMin, max: globalMax}];
    }
    if (placement === "shardDistributionShardListOnly") {
        request.shardDistribution = targetShards.map((shard) => ({shard}));
    }

    return {request, restrictedTo: placement === "none" ? [] : targetShards};
}

function reshardPlacementProperty({keyShape, scenario: {placement, targetShards}}) {
    const keyPattern = keyPatternFor(keyShape);

    // Must run before buildPlacementRequest()/resetZoneAssignment(): dropping the previous
    // iteration's collection here also clears its config.tags zone-range entry, so the zone
    // removal below doesn't see a stale "still in use by a chunk range" association left over
    // from the prior iteration and fail spuriously.
    setUpCollection();

    const {request, restrictedTo} = buildPlacementRequest(placement, targetShards, keyPattern);

    // A single chunk is enough for every scenario here: the zone/shardDistribution used below
    // always covers the full key range, and the fast/ShardDistribution paths ignore
    // numInitialChunks entirely. Without this, SamplingBasedSplitPolicy defaults to 90 initial
    // chunks, which kNumInitialDocs's cardinality can't satisfy.
    const commandObj = Object.assign(
        {reshardCollection: ns, key: keyPattern, numInitialChunks: 1},
        request,
    );
    assert.commandWorked(mongos.adminCommand(commandObj));

    const actualShards = new Set(
        findChunksUtil
            .findChunksByNs(configDB, ns)
            .toArray()
            .map((chunk) => chunk.shard),
    );

    if (restrictedTo.length > 0) {
        // The core invariant this test exists to guard: a placement request must be honored
        // regardless of which split policy served it.
        for (const shard of actualShards) {
            assert(
                restrictedTo.includes(shard),
                "reshardCollection placed data outside the requested placement",
                {commandObj, requestedShards: restrictedTo, actualShards: [...actualShards]},
            );
        }
    } else if (isHashedPrefix(keyPattern)) {
        // No placement was requested and the key is hashed-prefix: the fast path should spread
        // data across every shard. This characterizes the unaffected/intended fast-path
        // behavior, not a regression check.
        assert.eq(
            actualShards.size,
            shardNames.length,
            "hashed-prefix reshard without placement should use every shard",
            {commandObj, actualShards: [...actualShards]},
        );
    }
}

// The full cross-product of key shapes and placement scenarios, guaranteeing complete coverage
// deterministically (see the comment on 'modelArbitrary' above).
const examples = [];
for (const keyShape of ["range", "hashedPrefix", "hashedNonPrefix", "compoundRange"]) {
    for (const scenario of placementScenarios) {
        examples.push([{keyShape, scenario}]);
    }
}

describe("reshardCollection placement (zones/shardDistribution) property", function () {
    after(function () {
        testDB[collName].drop();
        resetZoneAssignment();
    });

    it("honors requested placement regardless of split policy", function () {
        const randomSweepSize = 10;
        fc.assert(fc.property(modelArbitrary, reshardPlacementProperty), {
            numRuns: examples.length + randomSweepSize,
            examples,
        });
    });
});
