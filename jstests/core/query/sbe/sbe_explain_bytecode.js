/**
 * Tests that the SBE bytecode is included in the explain output.
 *
 * @tags: [
 *    assumes_against_mongod_not_mongos,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    assumes_unsharded_collection,
 *    # We modify the value of a query knob. setParameter is not persistent.
 *    does_not_support_stepdowns,
 *    # Explain for the aggregate command cannot run within a multi-document transaction
 *    does_not_support_transactions,
 *    requires_fcv_83,
 * ]
 */

import {getEngine, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

db.c.drop();
db.c.insert({_id: 1, a: 1, b: 1});

db.d.drop();
db.d.insert({_id: 1, a: 1, b: 1});
db.d.createIndex({a: 1}, {collation: {locale: "en", strength: 2}});

runWithParamsAllNonConfigNodes(db, {"internalQueryFrameworkControl": "trySbeEngine"}, () => {
    const testCases = [
        {pipeline: [{$limit: 1}], options: {}},
        {pipeline: [{$match: {b: 1}}], options: {}},
        {pipeline: [{$match: {a: 1, b: 1}}], options: {}},
        {pipeline: [{$project: {a: 1}}], options: {}},
        {pipeline: [{$group: {_id: null, total: {$sum: "$a"}}}], options: {}},
        {
            pipeline: [
                {
                    $setWindowFields: {
                        partitionBy: "$a",
                        sortBy: {a: 1},
                        output: {res: {$sum: "$b", window: {documents: ["unbounded", "current"]}}},
                    },
                },
            ],
            options: {},
        },
        {pipeline: [{$lookup: {from: "d", localField: "a", foreignField: "a", as: "aa"}}], options: {}},
        {
            pipeline: [{$lookup: {from: "d", localField: "a", foreignField: "a", as: "aa"}}],
            options: {allowDiskUse: false},
        },
        {pipeline: [{$lookup: {from: "d", localField: "a", foreignField: "b", as: "ab"}}], options: {}},
    ];
    for (const tc of testCases) {
        const explain = db.c.explain("internal").aggregate(tc.pipeline, tc.options);
        jsTest.log({"explain": explain, "pipeline": tc.pipeline});
        const winningPlan = getQueryPlanner(explain).winningPlan;
        const slotBasedPlan = winningPlan.slotBasedPlan;
        if (getEngine(explain) === "sbe") {
            jsTest.log({"slotBasedPlan stages": slotBasedPlan.stages});
            assert(slotBasedPlan.stages.includes("stackSize"), "slotBasedPlan stages should have bytecode");
        }
    }

    db.c.createIndex({b: 1});

    for (const tc of testCases) {
        const explain = db.c.explain("internal").aggregate(tc.pipeline, tc.options);
        const winningPlan = getQueryPlanner(explain).winningPlan;
        const slotBasedPlan = winningPlan.slotBasedPlan;
        if (getEngine(explain) === "sbe") {
            jsTest.log({"slotBasedPlan stages": slotBasedPlan.stages});
            assert(slotBasedPlan.stages.includes("stackSize"), "slotBasedPlan stages should have bytecode");
        }
    }
});
