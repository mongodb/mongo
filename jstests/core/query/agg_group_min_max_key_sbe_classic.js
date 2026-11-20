/**
 * Tests that MinKey and MaxKey produced by a $group are returned as MinKey/MaxKey values rather
 * than as the object {"": MinKey} / {"": MaxKey}. This must hold for every query framework mode,
 * including the one where $group runs in SBE and the remaining stages run in the classic engine.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   # Explain does not support read concerns other than local.
 *   assumes_read_concern_local,
 *   assumes_read_concern_unchanged,
 *   assumes_stable_shard_list,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   # "Explain for the aggregate command cannot run within a multi-document transaction"
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   uses_explain,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {aggPlanHasStage, getEngine} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = db[jsTestName()];

const pipeline = [
    {$group: {_id: "$g", mx: {$max: "$v"}, mn: {$min: "$v"}}},
    {$project: {_id: 0, mx: 1, mn: 1, mxType: {$type: "$mx"}, mnType: {$type: "$mn"}}},
];

// assert.docEq compares BSON types, so this fails if MinKey/MaxKey are materialized as objects
// such as {"": MinKey}.
const expected = [{mx: MaxKey, mn: MinKey, mxType: "maxKey", mnType: "minKey"}];

// All possible values of the internalQueryFrameworkControl knob.
const frameworkModes = {
    forceClassicEngine: "classic",
    trySbeRestricted: "sbe",
    trySbeEngine: "sbe",
};

function setFrameworkControl(mode) {
    // Set the knob on every node: some suites (e.g. secondary_reads_passthrough) may route the
    // aggregation to a secondary, which must observe the same value.
    setParameterOnAllNonConfigNodes(db.getMongo(), "internalQueryFrameworkControl", mode);
}

describe("MinKey/MaxKey from an SBE $group followed by a classic stage", function () {
    let prevFrameworkControl;

    before(function () {
        prevFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;

        assert(coll.drop());
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, g: "k", v: MaxKey},
                {_id: 2, g: "k", v: MinKey},
            ]),
        );
    });

    after(function () {
        setFrameworkControl(prevFrameworkControl);
        assert(coll.drop());
    });

    it("returns the same result in every query framework mode", function () {
        for (const [mode, expectedEngine] of Object.entries(frameworkModes)) {
            setFrameworkControl(mode);

            const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
            const engine = getEngine(explain);
            assert.eq(expectedEngine, engine, "unexpected engine for framework mode", {mode});
            if (expectedEngine === "sbe") {
                assert(aggPlanHasStage(explain, "$project"), "expected a classic $project suffix", {
                    explain,
                });
            }

            const result = coll.aggregate(pipeline).toArray();
            jsTest.log.info("Running aggregation", {mode, engine});

            // Comparing every mode's result to the same expected value makes the results equal to
            // each other by transitivity.
            assert.docEq(expected, result, "unexpected aggregation result", {mode, engine});
        }
    });
});
