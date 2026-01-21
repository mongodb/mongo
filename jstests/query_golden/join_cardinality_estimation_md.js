//
// Test that the cardinality estimates for two-table joins approximate reality
//

import {checkJoinOptimizationStatus} from "jstests/libs/query/sbe_util.js";
import {checkPauseAfterPopulate} from "jstests/libs/pause_after_populate.js";

const joinOptimizationStatus = checkJoinOptimizationStatus(typeof db === "undefined" ? null : db);
if (!joinOptimizationStatus) {
    jsTest.log.info("Test requires enabled join optimization.");
    quit();
}

let goodEstimations = 0;
let badEstimations = 0;

function populate() {
    const collSize = 1000;

    const documents = [];
    for (let i = 0; i < collSize; i++) {
        documents.push({
            i_idx: i,
            i_noidx: i,
            i_idx_unique: i,
            c_idx: 1,
            d_idx: i % 10,
            i_idx_offset: i + 100000,
            n_idx: null,
        });
    }

    db.many_rows.insertMany(documents);
    db.many_rows.createIndex({i_idx: 1});
    db.many_rows.createIndex({i_idx_offset: 1});
    db.many_rows.createIndex({i_idx_unique: 1}, {unique: true});
    db.many_rows.createIndex({c_idx: 1});
    db.many_rows.createIndex({d_idx: 1});
    db.many_rows.createIndex({n_idx: 1});

    // An empty collection
    db.no_rows.createIndex({i_idx: 1});

    // Collection with a single row
    db.one_row.insert({i_idx: 1});
    db.one_row.createIndex({i_idx: 1});

    // Collection with 1 non-null document
    const nullDocuments = [];
    db.mostly_nulls.insert({i_idx: 1});
    for (let i = 0; i < collSize; i++) {
        nullDocuments.push({
            i_idx: null,
        });
    }
    db.mostly_nulls.insert(nullDocuments);
    db.mostly_nulls.createIndex({i_idx: 1});
}

function withinFactorOfTwo(a, b) {
    // Small numbers are close enough
    if (a < 5 && b < 5) {
        return true;
    }

    const ratio = a / b;
    return ratio >= 0.5 && ratio <= 2;
}
function estimateNoSubpipeline(left, right, localField, foreignField, pipelineMatch = {}) {
    estimatePipeline(left, [
        {$lookup: {from: right, localField: localField, foreignField: foreignField, as: "right"}},
        {$unwind: "$right"},
        {$match: pipelineMatch},
    ]);
}

function estimateWithSubpipeline(left, right, localField, foreignField, pipelineMatch = {}, subpipelineMatch = {}) {
    estimatePipeline(left, [
        {
            $lookup: {
                from: right,
                as: "right",
                let: {localField: "$" + localField},
                pipeline: [{$match: {$and: [{$expr: {$eq: ["$$localField", "$" + foreignField]}}, subpipelineMatch]}}],
            },
        },
        {$unwind: "$right"},
        {$match: pipelineMatch},
    ]);
}

function estimatePipeline(left, pipeline) {
    print("```js");
    print(`db.${left}.aggregate(${JSON.stringify(pipeline)})`);
    print("```");

    const execution = db.runCommand({
        aggregate: left,
        pipeline: [...pipeline, {"$count": "count"}],
        cursor: {},
    });
    let actualCardinality = 0;
    // $count returns no rows if the actual cardinality is zero
    if (execution.cursor.firstBatch[0] !== undefined) {
        actualCardinality = execution.cursor.firstBatch[0]["count"];
    }

    const explain = db.runCommand({
        explain: {aggregate: left, pipeline: pipeline, cursor: {}},
        verbosity: "executionStats",
    });
    assert(explain.queryPlanner !== undefined, JSON.stringify(explain));
    const cardinalityEstimate = explain.queryPlanner.winningPlan.queryPlan.cardinalityEstimate;
    assert(cardinalityEstimate !== undefined);

    // Trailing spaces ensure the text appears on separate lines.
    print(`Actual cardinality: ${actualCardinality}  `);
    print(`Estimated cardinality: ${cardinalityEstimate}  `);
    if (withinFactorOfTwo(actualCardinality, cardinalityEstimate)) {
        print(`Close enough?: yes`);
        goodEstimations++;
    } else {
        print(`Close enough?: NO`);
        badEstimations++;
    }
    print();
}

populate();
checkPauseAfterPopulate();

print("# One-to-one joins");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_idx");
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx");

print("# One-to-many joins");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "d_idx");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "c_idx");
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "d_idx");
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "c_idx");

print("# Many-to-one joins");
estimateNoSubpipeline("many_rows", "many_rows", "c_idx", "i_idx");
estimateWithSubpipeline("many_rows", "many_rows", "c_idx", "i_idx");

print("# Many-to-many joins");
estimateNoSubpipeline("many_rows", "many_rows", "d_idx", "d_idx");
estimateWithSubpipeline("many_rows", "many_rows", "d_idx", "d_idx");

print("# Cross joins");
estimateNoSubpipeline("many_rows", "many_rows", "c_idx", "c_idx");
estimateWithSubpipeline("many_rows", "many_rows", "c_idx", "c_idx");

// TODO SERVER-117385
print("# Joins on a unique index");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx_unique", "d_idx");
estimateNoSubpipeline("many_rows", "many_rows", "d_idx", "i_idx_unique");

print("# Join where there is an index only on one side");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_noidx");
estimateNoSubpipeline("many_rows", "many_rows", "i_noidx", "i_idx");

print("# Join with no indexes on either side");
estimateNoSubpipeline("many_rows", "many_rows", "i_noidx", "i_noidx");

print("# Join where no values match the join predicate");
estimateNoSubpipeline("many_rows", "many_rows", "c_idx", "i_idx_offset");

print("# Joins on an empty collection");
estimateNoSubpipeline("no_rows", "many_rows", "i_idx", "i_idx");
estimateNoSubpipeline("many_rows", "no_rows", "i_idx", "i_idx");
estimateNoSubpipeline("no_rows", "no_rows", "i_idx", "i_idx");

print("# Joins on a one-document input");
estimateNoSubpipeline("one_row", "many_rows", "i_idx", "i_idx");
estimateNoSubpipeline("many_rows", "one_row", "i_idx", "i_idx");

estimateNoSubpipeline("many_rows", "one_row", "i_idx", "i_idx", {i_idx: 1});
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {i_idx: 1});
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {i_idx: 1});
estimateWithSubpipeline("one_row", "many_rows", "i_idx", "i_idx", {}, {i_idx: 1});

print("# Join on a missing field");
estimateNoSubpipeline("many_rows", "many_rows", "missing_field", "i_idx");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "missing_field");
estimateNoSubpipeline("many_rows", "many_rows", "missing_field", "missing_field");

print("# Join on a field with all nulls");
estimateNoSubpipeline("many_rows", "many_rows", "n_idx", "i_idx");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "n_idx");
estimateNoSubpipeline("many_rows", "many_rows", "n_idx", "n_idx");

print("# Join on a field with mosly nulls");
estimateNoSubpipeline("many_rows", "mostly_nulls", "i_idx", "i_idx", {"i_idx": {$ne: null}});
estimateNoSubpipeline("mostly_nulls", "many_rows", "i_idx", "i_idx", {"i_idx": {$ne: null}});
estimateNoSubpipeline("mostly_nulls", "mostly_nulls", "i_idx", "i_idx", {"i_idx": {$ne: null}});

print("# Joins with filter on the left side over the join field");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {i_idx: 1});
estimateNoSubpipeline("many_rows", "many_rows", "d_idx", "i_idx", {d_idx: 1});
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {i_idx: {$lt: 50}});

print("# Joins with filter on the left side over another field (residual predicate)");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {d_idx: 1});
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {d_idx: {$lt: 5}});

print("# Joins with unsatisfiable filter on left side");
estimateNoSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {i_idx: {$lt: 0}});
estimateNoSubpipeline("many_rows", "many_rows", "c_idx", "i_idx", {c_idx: 0});

print("# Join with filters on the right side over the join field");
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {"i_idx": 1});
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {"i_idx": {$ne: 1}});
estimateWithSubpipeline("many_rows", "many_rows", "d_idx", "i_idx", {}, {"i_idx": 1});

print("# Join with filters on the right side over another field (residual predicate)");
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {"d_idx": 1});
estimateWithSubpipeline("many_rows", "many_rows", "d_idx", "d_idx", {}, {"i_idx": 1});

print("# Right side filter matches all rows");
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {"i_idx": {$gte: 0}});
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {"c_idx": 1});

print("# Right side filter matches no rows");
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {"i_idx": -999});
estimateWithSubpipeline("many_rows", "many_rows", "i_idx", "i_idx", {}, {"d_idx": 11});

print("# Multi-table joins - chain");
print("## Same join key throughout");
for (const predicate of [{}, {i_idx: 1}, {d_idx: 1}]) {
    estimatePipeline("many_rows", [
        {$lookup: {from: "many_rows", localField: "i_idx", foreignField: "i_idx", as: "right1"}},
        {$unwind: "$right1"},
        {$lookup: {from: "many_rows", localField: "i_idx", foreignField: "i_idx", as: "right2"}},
        {$unwind: "$right2"},
        {$match: predicate},
    ]);
}

print("## Different join keys");
for (const predicate of [{}, {i_idx: 1}, {d_idx: 1}]) {
    estimatePipeline("many_rows", [
        {$lookup: {from: "many_rows", localField: "i_idx", foreignField: "i_idx", as: "right1"}},
        {$unwind: "$right1"},
        {$lookup: {from: "many_rows", localField: "i_idx_offset", foreignField: "i_idx_offset", as: "right2"}},
        {$unwind: "$right2"},
        {$match: predicate},
    ]);
}

print("# Multi-table joins - star");
for (const predicate of [{}, {i_idx: 1}, {d_idx: 1}]) {
    estimatePipeline("many_rows", [
        {
            $lookup: {
                from: "many_rows",
                as: "right1",
                let: {localField: "$i_idx"},
                pipeline: [{$match: {$expr: {$eq: ["$$localField", "$i_idx"]}}}],
            },
        },
        {$unwind: "$right1"},
        {
            $lookup: {
                from: "many_rows",
                as: "right2",
                let: {localField: "$i_idx"},
                pipeline: [{$match: {$expr: {$eq: ["$$localField", "$i_idx"]}}}],
            },
        },
        {$unwind: "$right2"},
        {$match: predicate},
    ]);
}

print("# Multi-table joins - zero-cardinality tables at various positions");
estimatePipeline("no_rows", [
    {$lookup: {from: "many_rows", localField: "i_idx", foreignField: "i_idx", as: "right1"}},
    {$unwind: "$right1"},
    {$lookup: {from: "many_rows", localField: "i_idx_offset", foreignField: "i_idx_offset", as: "right2"}},
    {$unwind: "$right2"},
]);
estimatePipeline("many_rows", [
    {$lookup: {from: "no_rows", localField: "i_idx", foreignField: "i_idx", as: "right1"}},
    {$unwind: "$right1"},
    {$lookup: {from: "many_rows", localField: "i_idx_offset", foreignField: "i_idx_offset", as: "right2"}},
    {$unwind: "$right2"},
]);
estimatePipeline("many_rows", [
    {$lookup: {from: "many_rows", localField: "i_idx", foreignField: "i_idx", as: "right1"}},
    {$unwind: "$right1"},
    {$lookup: {from: "no_rows", localField: "i_idx_offset", foreignField: "i_idx_offset", as: "right2"}},
    {$unwind: "$right2"},
]);

print("# Multi-table joins - zero-cardinality predicates at various positions");
estimatePipeline("many_rows", [
    {
        $lookup: {
            from: "many_rows",
            as: "right1",
            let: {localField: "$i_idx"},
            pipeline: [{$match: {$expr: {$eq: ["$$localField", "$i_idx"]}}}],
        },
    },
    {$unwind: "$right1"},
    {
        $lookup: {
            from: "many_rows",
            as: "right2",
            let: {localField: "$i_idx"},
            pipeline: [{$match: {$expr: {$eq: ["$$localField", "$i_idx"]}}}],
        },
    },
    {$unwind: "$right2"},
    {$match: {d_idx: -1}},
]);

estimatePipeline("many_rows", [
    {
        $lookup: {
            from: "many_rows",
            as: "right1",
            let: {localField: "$i_idx"},
            pipeline: [
                {
                    $match: {
                        $expr: {$eq: ["$$localField", "$i_idx"]},
                        d_idx: -1,
                    },
                },
            ],
        },
    },
    {$unwind: "$right1"},
    {
        $lookup: {
            from: "many_rows",
            as: "right2",
            let: {localField: "$i_idx"},
            pipeline: [{$match: {$expr: {$eq: ["$$localField", "$i_idx"]}}}],
        },
    },
    {$unwind: "$right2"},
]);

estimatePipeline("many_rows", [
    {
        $lookup: {
            from: "many_rows",
            as: "right1",
            let: {localField: "$i_idx"},
            pipeline: [{$match: {$expr: {$eq: ["$$localField", "$i_idx"]}}}],
        },
    },
    {$unwind: "$right1"},
    {
        $lookup: {
            from: "many_rows",
            as: "right2",
            let: {localField: "$i_idx"},
            pipeline: [{$match: {$expr: {$eq: ["$$localField", "$i_idx"]}, d_idx: -1}}],
        },
    },
    {$unwind: "$right2"},
]);

print("# Summary");
print(`Good estimations: ${goodEstimations}  `);
print(`Bad estimations: ${badEstimations}  `);
