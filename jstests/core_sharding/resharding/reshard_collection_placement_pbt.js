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
import {kPbtInterruptAfterTimeLimitMs} from "jstests/libs/property_test_helpers/property_testing_utils.js";
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
const [shardA, shardB] = shardNames;

const kZoneName = "reshardPlacementPbtZone";
const kNumInitialDocs = 50;
// Pivot value used to split the key range in two for 'shardDistributionPartitioned'. Must fall
// strictly inside [0, kNumInitialDocs) so both halves receive seeded data.
const kPartitionPivot = 25;

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

// A boundary document for 'keyPattern' with 'pivotValue' in its first field and MinKey in every
// trailing field, so it can serve as both a chunk max (lower half) and min (upper half) and the
// two halves are exactly continuous.
function buildPartitionBoundary(keyPattern, pivotValue) {
    const boundary = {};
    let first = true;
    for (const field of Object.keys(keyPattern)) {
        boundary[field] = first ? pivotValue : MinKey;
        first = false;
    }
    return boundary;
}

const keyShapeArbitrary = fc.constantFrom(
    "range",
    "hashedPrefix",
    "hashedNonPrefix",
    "compoundRange",
);

// Shard-list-only targets, generalized to however many shards the cluster actually has (not just
// the first two). Every shard gets its own singleton target, so a split policy that ignores the
// shard list fails deterministically rather than depending on which shard it happens to pick; the
// larger prefixes (two shards up to all of them) follow.
const shardListTargets = shardNames.map((shard) => [shard]);
for (let count = 2; count <= shardNames.length; count++) {
    shardListTargets.push(shardNames.slice(0, count));
}

// Each scenario is a distinct combination of {zones, shardDistribution} that maps to a distinct
// branch of calculateParticipantShardsAndChunks:
//  - "none": no placement requested -> hashed-prefix keys take the zone-unaware fast path.
//  - "zonesOnly": zones alone -> SamplingBasedSplitPolicy (live aggregation dispatch).
//  - "shardDistributionMinMax": shardDistribution with min/max -> ShardDistributionSplitPolicy.
//  - "shardDistributionShardListOnly": shardDistribution without min/max ->
//    SamplingBasedSplitPolicy restricted to the listed shards (live aggregation dispatch).
//  - "zonesAndShardDistributionMinMax": both supplied and agreeing.
//  - "shardDistributionPartitioned": shardDistribution with min/max split across two shards ->
//    exercises ShardDistributionSplitPolicy's multi-range continuity handling, not just the
//    single-shard-covers-everything case the other scenarios use. Only meaningful for keys
//    without a hashed prefix (see isHashedPrefix() guard below), since a numeric pivot on a hashed
//    first field doesn't correspond to a meaningful split. A hashed non-prefix key is fine: its
//    ranged first field takes the pivot and the trailing hashed field takes MinKey.
const placementScenarios = [
    {placement: "none", targetShards: []},
    {placement: "zonesOnly", targetShards: [shardB]},
    {placement: "shardDistributionMinMax", targetShards: [shardB]},
    ...shardListTargets.map((targetShards) => ({
        placement: "shardDistributionShardListOnly",
        targetShards,
    })),
    {placement: "zonesAndShardDistributionMinMax", targetShards: [shardB]},
    {placement: "shardDistributionPartitioned", targetShards: [shardA, shardB]},
];

// Whether this reshardCollection call targets the shard key the collection is already sharded on
// (forceRedistribution: true, same-key redistribution), rather than changing the key. This
// exercises placement-honoring together with the forceRedistribution code path, which the FSM
// workloads' reshardCollectionSameKey state relies on but never combines with placement.
const modelArbitrary = fc.record({
    keyShape: keyShapeArbitrary,
    scenario: fc.constantFrom(...placementScenarios),
    sameKeyResharding: fc.boolean(),
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

function setUpCollection(initialKeyPattern) {
    testDB[collName].drop();
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: initialKeyPattern}));
    const bulk = testDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < kNumInitialDocs; i++) {
        bulk.insert({oldKey: i, newKey: i});
    }
    assert.commandWorked(bulk.execute());
}

// Builds the {zones, shardDistribution} request fields for 'placement'/'targetShards' against
// 'keyPattern', assigning zone tags as a side effect. Returns the request, the set of shards it
// restricts placement to (empty if unrestricted), and, when the request fully specifies the
// resulting chunks, those chunks in ascending 'min' order (null otherwise).
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
    if (placement === "shardDistributionPartitioned") {
        const pivot = buildPartitionBoundary(keyPattern, kPartitionPivot);
        request.shardDistribution = [
            {shard: targetShards[0], min: globalMin, max: pivot},
            {shard: targetShards[1], min: pivot, max: globalMax},
        ];
    }

    return {
        request,
        restrictedTo: placement === "none" ? [] : targetShards,
        expectedChunks:
            placement === "shardDistributionPartitioned"
                ? request.shardDistribution.map(({shard, min, max}) => ({min, max, shard}))
                : null,
    };
}

function reshardPlacementProperty({
    keyShape,
    scenario: {placement, targetShards},
    sameKeyResharding,
}) {
    const keyPattern = keyPatternFor(keyShape);

    // 'shardDistributionPartitioned' splits the range at a numeric pivot, which only corresponds
    // to a meaningful split for keys without a hashed prefix.
    fc.pre(placement !== "shardDistributionPartitioned" || !isHashedPrefix(keyPattern));

    // When sameKeyResharding, the collection starts out already sharded on 'keyPattern' so the
    // reshardCollection call below is a true same-key redistribution; otherwise it starts on the
    // original {oldKey: 1} shard key and reshardCollection changes it to 'keyPattern', as before.
    //
    // Must run before buildPlacementRequest()/resetZoneAssignment(): dropping the previous
    // iteration's collection here also clears its config.tags zone-range entry, so the zone
    // removal below doesn't see a stale "still in use by a chunk range" association left over
    // from the prior iteration and fail spuriously.
    setUpCollection(sameKeyResharding ? keyPattern : {oldKey: 1});

    const {request, restrictedTo, expectedChunks} = buildPlacementRequest(
        placement,
        targetShards,
        keyPattern,
    );

    // A single chunk is enough for every scenario here: the zone/shardDistribution used below
    // always covers the full key range (or, for 'shardDistributionPartitioned', exactly two
    // explicit ranges), and the fast/ShardDistribution paths ignore numInitialChunks entirely.
    // Without this, SamplingBasedSplitPolicy defaults to 90 initial chunks, which
    // kNumInitialDocs's cardinality can't satisfy.
    const commandObj = Object.assign(
        {reshardCollection: ns, key: keyPattern, numInitialChunks: 1},
        request,
    );
    if (sameKeyResharding) {
        commandObj.forceRedistribution = true;
        // TODO(SERVER-131275): Remove reshardingUUID once the reshardCollectionCoordinator
        // supports retryability.
        commandObj.reshardingUUID = UUID();
    }
    assert.commandWorked(mongos.adminCommand(commandObj));

    const actualChunks = findChunksUtil
        .findChunksByNs(configDB, ns)
        .sort({min: 1})
        .toArray()
        .map((chunk) => ({min: chunk.min, max: chunk.max, shard: chunk.shard}));
    const actualShards = new Set(actualChunks.map((chunk) => chunk.shard));

    if (expectedChunks) {
        // The request spelled out every range and its shard, so the resulting chunks must match
        // exactly: not merely stay within the allowed shards, but not collapse onto one shard or
        // swap the ranges either.
        assert.docEq(
            expectedChunks,
            actualChunks,
            "reshardCollection did not honor the requested shardDistribution",
            {commandObj},
        );
    } else if (restrictedTo.length > 0) {
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

// The full cross-product of key shapes and placement scenarios (with sameKeyResharding: false),
// guaranteeing complete coverage deterministically (see the comment on 'modelArbitrary' above).
// 'shardDistributionPartitioned' is skipped for hashed-prefix keys, matching the fc.pre() guard in
// the property.
const examples = [];
for (const keyShape of ["range", "hashedPrefix", "hashedNonPrefix", "compoundRange"]) {
    for (const scenario of placementScenarios) {
        if (
            scenario.placement === "shardDistributionPartitioned" &&
            isHashedPrefix(keyPatternFor(keyShape))
        ) {
            continue;
        }
        examples.push([{keyShape, scenario, sameKeyResharding: false}]);
    }
}
// One sameKeyResharding: true example per key shape, pinning the forceRedistribution + placement
// combination deterministically.
for (const keyShape of ["range", "hashedPrefix", "hashedNonPrefix", "compoundRange"]) {
    examples.push([
        {
            keyShape,
            scenario: {placement: "shardDistributionMinMax", targetShards: [shardB]},
            sameKeyResharding: true,
        },
    ]);
}

describe("reshardCollection placement (zones/shardDistribution) property", function () {
    after(function () {
        testDB[collName].drop();
        resetZoneAssignment();
    });

    it("honors requested placement regardless of split policy", function () {
        // Run only the deterministic examples: each iteration is a full reshardCollection, and
        // adding a random sweep on top pushed the test past the burn-in per-test timeout.
        fc.assert(fc.property(modelArbitrary, reshardPlacementProperty), {
            numRuns: examples.length,
            examples,
            interruptAfterTimeLimit: kPbtInterruptAfterTimeLimitMs,
        });
    });
});
