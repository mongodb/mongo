/* Tests for the query-wide spilling totals (totalSpills, totalSpilledBytes, totalSpilledRecords and
 * totalSpilledDataStorageSize) in explain output through mongos.
 *
 * For find (and count) explains mongos sums the totals reported by each shard into the top-level
 * 'executionStats' and onto the root mongos stage. For aggregate explains mongos does not sum them,
 * so the totals only appear inside each shard's explain under 'shards.<shardId>'.
 *
 * @tags: [requires_fcv_91]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const spillingFields = ["spills", "spilledBytes", "spilledRecords", "spilledDataStorageSize"];
const totalSpillingFields = [
    "totalSpills",
    "totalSpilledBytes",
    "totalSpilledRecords",
    "totalSpilledDataStorageSize",
];

// Maps a per-stage metric name to its query-wide total, e.g. 'spills' -> 'totalSpills'.
function totalFieldName(field) {
    return "total" + field.charAt(0).toUpperCase() + field.slice(1);
}

// Spilling memory thresholds set on every shard so that a blocking sort or a $group spills for any
// non-empty input.
const spillingKnobs = {
    // Spilling memory threshold for a blocking SORT.
    internalQueryMaxBlockingSortMemoryUsageBytes: 1,
    // Spilling memory threshold for $group.
    internalDocumentSourceGroupMaxMemoryBytes: 1,
    internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1,
};

const st = new ShardingTest({name: jsTestName(), shards: 2, rs: {nodes: 1}});
const shardPrimaries = [st.rs0.getPrimary(), st.rs1.getPrimary()];
// The original knob values of each shard, restored before the cluster is stopped so that the
// shutdown consistency checks do not run with the tiny spilling thresholds.
const originalKnobs = new Map();

function setShardParameters(conn, params) {
    assert.commandWorked(conn.adminCommand({setParameter: 1, ...params}));
}

const mongosDB = st.s.getDB("test");
const coll = mongosDB[jsTestName()];

before(function () {
    for (const conn of shardPrimaries) {
        const original = {};
        for (const knob of Object.keys(spillingKnobs)) {
            const res = assert.commandWorked(conn.adminCommand({getParameter: 1, [knob]: 1}));
            original[knob] = res[knob];
        }
        originalKnobs.set(conn, original);
        setShardParameters(conn, spillingKnobs);
    }

    st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});

    // Insert documents with a non-indexed field 'a', so that a sort on 'a' is a blocking SORT.
    const docs = [];
    for (let i = -25; i < 25; i++) {
        docs.push({_id: i, a: i, padding: "some padding to make the documents bigger".repeat(2)});
    }
    assert.commandWorked(coll.insert(docs));
});

after(function () {
    for (const [conn, original] of originalKnobs) {
        setShardParameters(conn, original);
    }
    st.stop();
});

// Walks the explain tree and sums the per-stage spilling metrics over all stages that report them.
// The counters can be plain numbers or NumberLongs, so do not check the type.
function sumPerStageSpillingStats(
    node,
    sums = Object.fromEntries(spillingFields.map((f) => [f, 0])),
) {
    if (Array.isArray(node)) {
        node.forEach((child) => sumPerStageSpillingStats(child, sums));
    } else if (node !== null && typeof node === "object") {
        if (node.hasOwnProperty("spills") && !isNaN(Number(node.spills))) {
            for (const field of spillingFields) {
                sums[field] += Number(node[field]);
            }
        }
        for (const key of Object.keys(node)) {
            sumPerStageSpillingStats(node[key], sums);
        }
    }
    return sums;
}

// Returns true if any stage in the explain tree reports per-stage spilling metrics.
function hasPerStageSpillingStats(node) {
    if (Array.isArray(node)) {
        return node.some(hasPerStageSpillingStats);
    }
    if (node !== null && typeof node === "object") {
        return node.hasOwnProperty("spills") || Object.values(node).some(hasPerStageSpillingStats);
    }
    return false;
}

// A shard entry under 'executionStats.executionStages.shards' only carries the total* fields when
// that shard actually spilled, so a non-spilling shard contributes 0.
function shardTotal(shardEntry, field) {
    return shardEntry.hasOwnProperty(field) ? Number(shardEntry[field]) : 0;
}

// Asserts that 'totalsHolder' reports the spilling totals and that they equal the sums of the
// per-stage spilling metrics found under 'stagesRoot'.
function assertTotalsMatchPerStageSums(totalsHolder, stagesRoot, shardId, shardExplain) {
    const perStageSums = sumPerStageSpillingStats(stagesRoot);
    assert.gt(perStageSums.spills, 0, `Expected shard "${shardId}" to spill`, {shardExplain});
    for (const field of spillingFields) {
        const totalField = totalFieldName(field);
        assert(
            totalsHolder.hasOwnProperty(totalField),
            `Missing "${totalField}" in the explain of shard "${shardId}" although it spilled`,
            {shardExplain},
        );
        assert.eq(
            Number(totalsHolder[totalField]),
            perStageSums[field],
            `The "${totalField}" of shard "${shardId}" must equal the sum of its per-stage "${field}"`,
            {shardExplain},
        );
    }
}

// mongos does not sum the shards' totals for aggregates.
function assertNoMongosAggregateTotals(explain) {
    for (const field of totalSpillingFields) {
        assert(
            !explain.hasOwnProperty(field),
            `mongos must not report "${field}" at the top level of an aggregate explain`,
            {explain},
        );
    }
}

describe("mongos explain spilling totals", function () {
    it("sums the shards' spilling totals for a spilling find", function () {
        const explain = coll.find().sort({a: 1}).explain("executionStats");
        const execStats = explain.executionStats;
        const rootStage = execStats.executionStages;
        const shardEntries = rootStage.shards;
        assert.eq(2, shardEntries.length, "Expected the explain to hit both shards", {explain});

        for (const field of totalSpillingFields) {
            assert(
                execStats.hasOwnProperty(field),
                `Missing "${field}" in the top-level mongos executionStats although it spills`,
                {explain},
            );
            assert.gt(
                Number(execStats[field]),
                0,
                `Expected a positive "${field}" in the top-level mongos executionStats`,
                {explain},
            );

            // The root mongos stage must report the same totals.
            assert(rootStage.hasOwnProperty(field), `Missing "${field}" on the root mongos stage`, {
                explain,
            });
            assert.eq(
                Number(rootStage[field]),
                Number(execStats[field]),
                `Unexpected "${field}" on the root mongos stage`,
                {explain},
            );

            // The mongos totals must equal the sum of the shards' own totals.
            const shardSum = shardEntries.reduce((acc, entry) => acc + shardTotal(entry, field), 0);
            assert.eq(
                Number(execStats[field]),
                shardSum,
                `The mongos "${field}" must equal the sum of the shards' totals`,
                {explain},
            );
        }
    });

    it("matches the shards' totals to the per-stage spilling metrics for a find", function () {
        const explain = coll.find().sort({a: 1}).explain("executionStats");
        for (const shardEntry of explain.executionStats.executionStages.shards) {
            const perStageSums = sumPerStageSpillingStats(shardEntry.executionStages);
            assert.gt(perStageSums.spills, 0, "Expected the shard's plan to spill", {shardEntry});
            for (const field of spillingFields) {
                const msg =
                    `The shard's "${totalFieldName(field)}" must equal the sum of its ` +
                    `per-stage "${field}"`;
                assert.eq(shardTotal(shardEntry, totalFieldName(field)), perStageSums[field], msg, {
                    shardEntry,
                });
            }
        }
    });

    // A blocking sort makes every shard spill, so the same find with and without the sort is a
    // positive and a negative control: every spilling field must be present exactly when it sorts.
    for (const withSort of [true, false]) {
        it(`reports the spilling fields only when the find spills (withSort: ${withSort})`, function () {
            const cursor = withSort ? coll.find().sort({a: 1}) : coll.find();
            const explain = cursor.explain("executionStats");
            const execStats = explain.executionStats;
            const rootStage = execStats.executionStages;
            assert.eq(2, rootStage.shards.length, "Expected the explain to hit both shards", {
                explain,
            });

            for (const field of totalSpillingFields) {
                assert.eq(
                    withSort,
                    execStats.hasOwnProperty(field),
                    `Unexpected presence of "${field}" in the top-level mongos executionStats`,
                    {explain},
                );
                assert.eq(
                    withSort,
                    rootStage.hasOwnProperty(field),
                    `Unexpected presence of "${field}" on the root mongos stage`,
                    {explain},
                );
                for (const shardEntry of rootStage.shards) {
                    assert.eq(
                        withSort,
                        shardEntry.hasOwnProperty(field),
                        `Unexpected presence of "${field}" in a shard's explain`,
                        {shardEntry},
                    );
                }
            }
            for (const shardEntry of rootStage.shards) {
                assert.eq(
                    withSort,
                    hasPerStageSpillingStats(shardEntry.executionStages),
                    "Unexpected presence of per-stage spilling metrics in a shard's plan",
                    {shardEntry},
                );
            }
        });
    }

    // A $sort on a non-indexed field is pushed down into the query executor in both the classic
    // engine and SBE, so each shard returns a find-shaped explain with the totals in its
    // 'executionStats'.
    it("reports per-shard totals for a spilling $sort aggregate without summing them", function () {
        const explain = coll
            .explain("executionStats")
            .aggregate([{$sort: {a: 1}}], {allowDiskUse: true});
        assert(explain.hasOwnProperty("shards"), "Expected per-shard explains", {explain});

        for (const [shardId, shardExplain] of Object.entries(explain.shards)) {
            assert(
                shardExplain.hasOwnProperty("executionStats") &&
                    !shardExplain.hasOwnProperty("stages"),
                `Expected a find-shaped explain from shard "${shardId}"`,
                {shardExplain},
            );
            assertTotalsMatchPerStageSums(
                shardExplain.executionStats,
                shardExplain.executionStats.executionStages,
                shardId,
                shardExplain,
            );
        }
        assertNoMongosAggregateTotals(explain);
    });

    // '$_internalInhibitOptimization' keeps the $group out of the query executor (it would
    // otherwise be pushed down into SBE and give a find-shaped explain), so each shard returns a
    // pipeline-shaped explain whose totals sum the stats of all of its stages.
    it("reports per-shard totals for a spilling $group aggregate without summing", function () {
        const explain = coll
            .explain("executionStats")
            .aggregate([{$_internalInhibitOptimization: {}}, {$group: {_id: "$a", n: {$sum: 1}}}], {
                allowDiskUse: true,
            });
        assert(explain.hasOwnProperty("shards"), "Expected per-shard explains", {explain});

        for (const [shardId, shardExplain] of Object.entries(explain.shards)) {
            assert(
                shardExplain.hasOwnProperty("stages"),
                `Expected a pipeline-shaped explain from shard "${shardId}"`,
                {shardExplain},
            );
            assertTotalsMatchPerStageSums(shardExplain, shardExplain.stages, shardId, shardExplain);
        }
        assertNoMongosAggregateTotals(explain);
    });
});
