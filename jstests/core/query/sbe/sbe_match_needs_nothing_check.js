/**
 * Tests that SBE builds traverseF instructions in leading $match when the filter needs nothing check, even when path arrayness information is available.
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
 *    featureFlagPathArrayness
 * ]
 */

import {getEngine, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";

const docs = [
    {
        "_id": 105,
        "m": {
            "m1": 42,
            "m2": 42,
        },
        "array": 42,
        "a": 42,
        "b": 42,
    },
];

const indexes = [
    {
        "def": {
            "m.m2": 1,
        },
        "options": {},
    },
    {
        "def": {
            "$**": 1,
        },
        "options": {
            "wildcardProjection": {
                "m": 1,
            },
        },
    },
];

const queries = [
    {
        "pipeline": [
            {
                "$match": {
                    "$and": [
                        {
                            "$and": [
                                {
                                    "a": {
                                        "$elemMatch": {
                                            "$and": [{"m.m2": {"$in": [null]}, "a": {"$eq": NumberInt(0)}}],
                                            "a": {"$eq": NumberInt(0)},
                                        },
                                    },
                                },
                                {"_id": {"$eq": NumberInt(0)}, "a": {"$eq": NumberInt(0)}},
                            ],
                            "a": {"$eq": NumberInt(0)},
                        },
                    ],
                    "a": {"$eq": NumberInt(0)},
                },
            },
            {"$group": {"_id": null}},
            {"$project": {"_id": 0, "a": 1}},
        ],
    },
];

db.c.drop();
db.c.insertMany(docs);
for (let index of indexes) {
    db.c.createIndex(index.def, index.options);
}
for (let query of queries) {
    const explain = db.c.explain("executionStats").aggregate(query.pipeline, query.options);
    const engine = getEngine(explain);
    jsTest.log({"explain": explain});
    if (engine === "sbe") {
        const filterStages = getSbePlanStages(explain, "filter");
        assert.eq(filterStages.length, 1, "Should have one filter stage");
        assert(
            filterStages[0].filter.includes("traverseF"),
            `Leading $match that needs nothing check should use traverseF. Filter: ${filterStages[0].filter}`,
        );
    }
}
