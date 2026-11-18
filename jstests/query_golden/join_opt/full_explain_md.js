/**
 * Records the queryPlanner explain output of a join-optimized query as users see it, so that any
 * change to its format shows up as a readable diff.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {code, line, linebreak, section} from "jstests/libs/query/pretty_md.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {joinTestWrapper} from "jstests/libs/query/join_utils.js";
import {normalizeRunVarying} from "jstests/libs/query/analyze_plan.js";

const shop = db.getSiblingDB("shop");
assert.commandWorked(shop.dropDatabase());

const tiers = ["gold", "silver", "bronze", "copper", "iron", "nickel", "tin", "zinc"];
const countries = ["BE", "NL", "FR", "DE", "UK"];
const categories = ["books", "toys", "garden", "tools", "music"];
const regions = ["EMEA", "APAC", "AMER"];
const statuses = ["shipped", "pending", "cancelled", "returned"];

assert.commandWorked(
    shop.customers.insertMany(
        Array.from({length: 100}, (_, i) => ({
            _id: i,
            tier: tiers[i % tiers.length],
            active: i % 3 != 0,
            country: countries[i % countries.length],
        })),
    ),
);
assert.commandWorked(
    shop.suppliers.insertMany(
        Array.from({length: 6}, (_, i) => ({_id: i, code: `S${i}`, region: regions[i % 3]})),
    ),
);
assert.commandWorked(
    shop.products.insertMany(
        Array.from({length: 50}, (_, i) => ({
            _id: i,
            category: categories[i % categories.length],
            supplierCode: `S${i % 6}`,
        })),
    ),
);
assert.commandWorked(
    shop.orders.insertMany(
        Array.from({length: 1000}, (_, i) => ({
            _id: i,
            customerId: (i * 7) % 100,
            productId: (i * 3) % 50,
            status: statuses[i % statuses.length],
            total: (i * 37) % 250,
        })),
    ),
);
assert.commandWorked(shop.customers.createIndex({tier: 1}));
assert.commandWorked(shop.orders.createIndexes([{customerId: 1}, {productId: 1}]));
assert.commandWorked(shop.products.createIndex({supplierCode: 1}));
assert.commandWorked(shop.suppliers.createIndex({code: 1}));

const pipeline = [
    {$match: {status: "shipped", total: {$gte: 100}}},
    {$lookup: {from: "customers", localField: "customerId", foreignField: "_id", as: "cust"}},
    {$unwind: "$cust"},
    {$lookup: {from: "products", localField: "productId", foreignField: "_id", as: "prod"}},
    {$unwind: "$prod"},
    {
        $lookup: {
            from: "suppliers",
            localField: "prod.supplierCode",
            foreignField: "code",
            as: "supp",
        },
    },
    {$unwind: "$supp"},
    {
        $match: {
            "cust.tier": "gold",
            "cust.active": true,
            "prod.category": {$in: ["books", "toys"]},
            "supp.region": "EMEA",
        },
    },
    {$group: {_id: "$cust.country", revenue: {$sum: "$total"}}},
];

// Force each join method (INLJ, HJ, NLJ) to ensure coverage of each join method.
// We also use "ALL" enumeration mode to force the existence of rejected plans.
const joinHint = {
    perSubsetLevelMode: [
        {level: NumberInt(0), mode: "CHEAPEST", hint: {node: NumberInt(1)}},
        {level: NumberInt(1), mode: "ALL", hint: {node: NumberInt(0), method: "INLJ"}},
        {
            level: NumberInt(2),
            mode: "ALL",
            hint: {node: NumberInt(2), method: "HJ", isLeftChild: false},
        },
        {
            level: NumberInt(3),
            mode: "ALL",
            hint: {node: NumberInt(3), method: "NLJ", isLeftChild: false},
        },
    ],
};
const hintedPipeline = [{$_internalJoinHint: joinHint}].concat(pipeline);

// These fields should technically be consistent across runs, but they distract from the purpose of
// the test. For example, a cost coefficient change should not change the golden output here.
// The fields will still be present in explain, but their values replaced with "<normalized>".
const kNormalizedFields = [
    "slotBasedPlan",
    "executionStages",
    "planCacheKey",
    "costEstimate",
    "cardinalityEstimate",
    "cardinalityRHSBeforeJoinPred",
    // In 'ndvEstimates'.
    "ndv",
    "selectivity",
    // In 'joinCostComponents'.
    "docsProcessed",
    "docsOutput",
    "numDocsTransmitted",
    "sequentialIOPages",
    "randomIOPages",
    "localOpCost",
    "totalCost",
];

function normalizedExplain(verbosity, options = {}) {
    const output = shop.orders.explain(verbosity).aggregate(hintedPipeline, options);
    return normalizeRunVarying(output, kNormalizedFields);
}

joinTestWrapper(db, () => {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    const expected = shop.orders.aggregate(pipeline).toArray();
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    const actual = shop.orders.aggregate(hintedPipeline).toArray();
    assertArrayEq({expected, actual});

    section("Query");
    code(tojson(pipeline));
    line("Results:");
    code(tojson(actual.sort((l, r) => l._id.localeCompare(r._id))));
    line(
        "Explain values that change between runs or are covered by other tests are replaced " +
            'with `"<normalized>"`.',
    );
    linebreak();

    section("queryPlanner verbosity");
    const queryPlannerExplain = normalizedExplain("queryPlanner");
    code(tojson(queryPlannerExplain.queryPlanner));

    section("queryPlanner verbosity with enableDetailedJoinExplain");
    const detailedExplain = normalizedExplain("queryPlanner", {
        querySettings: {queryKnobs: {enableDetailedJoinExplain: true}},
    });
    code(tojson(detailedExplain.queryPlanner));

    for (const verbosity of ["executionStats", "allPlansExecution"]) {
        assert.docEq(
            queryPlannerExplain.queryPlanner,
            normalizedExplain(verbosity).queryPlanner,
            `'queryPlanner' differs between 'queryPlanner' and '${verbosity}' verbosities`,
        );
    }
});

assert.commandWorked(shop.dropDatabase());
