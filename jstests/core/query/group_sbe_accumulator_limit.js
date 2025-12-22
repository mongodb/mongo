/**
 * Tests that the determination to use SBE for $group stages is made according to the
 * internalMaxGroupAccumulatorsInSbe parameter and engine configuration.
 *
 * @tags: [
 *   # Sharded collection passthroughs add shard filters that can prevent $group pushdown when it
 *   # would otherwise be allowed.
 *   assumes_unsharded_collection,
 *
 *   # "Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results."
 *   does_not_support_stepdowns,
 *
 *   # "Cannot insert into a time-series collection in a multi-document transaction."
 *   does_not_support_transactions,
 *
 *   # This CRUD passthrough mutates pipelines in ways that interfere with SBE compatibility
 *   # decisions.
 *   exclude_from_timeseries_crud_passthrough,
 *
 *   # The accumulator limit being tested here is feature-flag gated.
 *   featureFlagSbeAccumulators,
 *
 *   # Nodes in the initial sync state may ignore getParameter commands, causing the test to hang.
 *   incompatible_with_initial_sync,
 *
 *   requires_fcv_83,
 *   writes_timeseries_collection,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getAllNodeExplains, getPlanStages} from "jstests/libs/query/analyze_plan.js";
import {checkSbeStatus, kFeatureFlagSbeFullEnabled, kSbeDisabled} from "jstests/libs/query/sbe_util.js";

/*
 * The 'internalMaxGroupAccumulatorsInSbe' query knob is 16 by default. Check that nothing in the
 * environment has modified it.
 */
const expectedMaxGroupAccumulatorsInSbe = 16;

const maxGroupAccumulatorsInSbeParameterValues = FixtureHelpers.mapOnEachShardNode({
    db: db.getSiblingDB("admin"),
    func: (adminDB) =>
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalMaxGroupAccumulatorsInSbe: 1}))
            .internalMaxGroupAccumulatorsInSbe,
});

assert(
    maxGroupAccumulatorsInSbeParameterValues.every((value) => value == expectedMaxGroupAccumulatorsInSbe),
    maxGroupAccumulatorsInSbeParameterValues,
);

const sbeStatus = checkSbeStatus(db);

/*
 * Populate a regular collection and a time-series collection with sample data, so we can test the
 * effect of the 'internalMaxGroupAccumulatorsInSbe' parameter on both collection types.
 */

const coll = db.getSiblingDB(jsTestName())["regular"];
coll.drop();

assert.commandWorked(coll.insert(Array.from({length: 10}, (_, i) => ({_id: i, group: i, value: i}))));

const tsColl = db.getSiblingDB(jsTestName())["timeseries"];
tsColl.drop();
tsColl.getDB().createCollection(tsColl.getName(), {timeseries: {timeField: "ts"}});
const firstTimeStamp = ISODate("2000-01-01T00:00:00Z");
assert.commandWorked(
    tsColl.insert(
        Array.from({length: 10}, (_, i) => ({
            ts: new Date(firstTimeStamp.getTime() + i * 1000),
            group: i,
            value: i,
        })),
    ),
);

/**
 * Use the explain plan for a $group with n accumulators to determine which engine it executes in,
 * and verify that the engine choice respects the $group accumulator limit (e.g., the
 * 'internalMaxGroupAccumulatorsInSbe' parameter), as well as any overriding engine parameters.
 */
function testGroupWithNAccumulators({coll, n, expectedLimit}) {
    /*
     * We determine which engine the $group executes in by checking for a GROUP stage in the query's
     * explain plan, indicating the planner's intention to compile the group operation in SBE.
     *
     * When SBE is allowed, we expect to see the GROUP stage iff the number of accumulators n is no
     * more than the SBE eligibility limit or the accumulator limit is overridden by
     * featureFlagSbeFull.
     *
     * Converseely, we do not expect to see a GROUP stage when the number of accumulators exceeds
     * the limit or when SBE is explicitly disabled.
     */
    const expectedGroupNodes =
        sbeStatus === kSbeDisabled ? 0 : sbeStatus === kFeatureFlagSbeFullEnabled ? 1 : n <= expectedLimit ? 1 : 0;

    jsTest.log.info("Testing $group with n accumulators: ", {
        collection: coll.getName(),
        n,
        expectedLimit,
        expectedEngine: expectedGroupNodes === 0 ? "classic" : "sbe",
    });

    // Construct the body of a $group stage with 'n' accumulator expressions.
    const groupStageBody = Array.from({length: n}, (_, i) => ({[`accumulator${i}`]: {$sum: `$value`}})).reduce(
        (group, accumulator) => Object.assign(group, accumulator),
        {_id: "$group"},
    );

    // Run the aggregation and check the number of GROUP nodes in the explain plan.
    const explain = assert.commandWorked(coll.explain().aggregate([{$group: groupStageBody}]));
    getAllNodeExplains(explain).forEach((nodeExplain) => {
        const groupStages = getPlanStages(nodeExplain, "GROUP");
        assert.eq(groupStages.length, expectedGroupNodes, explain);
    });
}

for (const n of [1, expectedMaxGroupAccumulatorsInSbe, expectedMaxGroupAccumulatorsInSbe + 1]) {
    // For regular collections, we expect the planner to enforce the
    // 'internalMaxGroupAccumulatorsInSbe' limit.
    testGroupWithNAccumulators({coll, n, expectedLimit: expectedMaxGroupAccumulatorsInSbe});

    // For time-series collections, we expect the planner to ignore the limit.
    testGroupWithNAccumulators({coll: tsColl, n, expectedLimit: Infinity});
}
