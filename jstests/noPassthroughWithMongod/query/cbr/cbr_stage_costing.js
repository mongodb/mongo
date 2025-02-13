/**
 * Test the costs of individual plan stages. Rather than test the
 * absolute costs, which may change on recalibration, we rather assert
 * on the general principles that should always hold.
 *
 */

import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];

coll.drop();
let docs = [];
for (let i = 0; i < 5000; i++) {
    docs.push({a: i, a_multikey: [i], b: i});
}

coll.insert(docs);
coll.createIndex({a: 1});
coll.createIndex({a_multikey: 1});
coll.createIndex({b: 1});
coll.createIndex({a: 1, b: 1});
coll.createIndex({b: 1, a: 1});
for (let fieldName of ["a", "a_multikey", "b", "c", "d", "e"]) {
    db.coll.runCommand({analyze: collName, key: fieldName});
}

const collOneRowName = collName + "_one_row";
const collOneRow = db[collOneRowName];
collOneRow.insert({a: 1, b: 1});
collOneRow.runCommand({analyze: collOneRowName, key: "a"});
collOneRow.createIndex({a: 1});

const collZeroRowsName = collName + "_zero_rows";
const collZeroRows = db[collZeroRowsName];
collZeroRows.createIndex({a: 1});
collZeroRows.runCommand({analyze: collZeroRows, key: "a"});

/**
 * Extracts the complete cost of the winning plan
 */
function planCost(cursor) {
    const explain = cursor.explain();
    const winningPlan = getWinningPlanFromExplain(explain);
    return winningPlan.costEstimate;
}

/**
 * Extract the cost of the top stage only (minus its inputs)
 */
function rootStageCost(cursor) {
    const explain = cursor.explain();
    const winningPlan = getWinningPlanFromExplain(explain);

    if (winningPlan.inputStage) {
        const rootStageCost = winningPlan.costEstimate - winningPlan.inputStage.costEstimate;
        if (winningPlan.stage !== "LIMIT") {
            assert.gt(rootStageCost,
                      0,
                      "root stage cost is expected to be greater than the inputStage cost");
        }
        return rootStageCost;
    } else {
        return winningPlan.costEstimate;
    }
}

/**
 * Extract the cost of the IXSCAN stage out of the explain().
 */
function ixscanCost({predicate, hint}) {
    assert(hint !== null);
    const explain = coll.find(predicate).hint(hint).explain();
    const ixScan = getPlanStage(explain, "IXSCAN");
    assert.isnull(ixScan.inputStage);
    return ixScan.costEstimate;
}

function runTest(planRankerMode) {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: planRankerMode}));

    /*
     * Cost of COLLSCAN
     */

    // COLLSCAN cost for an empty collection should be small.
    assert.lt(rootStageCost(collZeroRows.find().hint({$natural: 1})), 0.0001);

    // COLLSCAN cost is dependent on input collection size.
    assert.gt(rootStageCost(coll.find().hint({$natural: 1})),
              rootStageCost(collOneRow.find().hint({$natural: 1})));

    // COLLSCAN cost should be smaller if no filter.
    assert.lt(rootStageCost(coll.find().hint({$natural: 1})),
              rootStageCost(coll.find({a: 1}).hint({$natural: 1})));

    // COLLSCAN cost is dependent on the number of predicates.
    assert.gt(rootStageCost(coll.find({a: 1, b: 1, c: 1, d: 1, e: 1}).hint({$natural: 1})),
              rootStageCost(coll.find({a: 1}).hint({$natural: 1})));

    // COLLSCAN cost is not dependent on the selectivity of the predicate
    assert.eq(rootStageCost(coll.find({a: {$gt: 1}}).hint({$natural: 1})),
              rootStageCost(coll.find({a: {$gt: 1000}}).hint({$natural: 1})));

    // COLLSCAN with filter (cardinality estimate using the current CE method)
    // has approximately same cost as COLLSCAN with no filter (cardinality estimate from Metadata).
    assert.lt(rootStageCost(coll.find({a: {$gt: 1}}).hint({$natural: 1})) -
                  rootStageCost(coll.find({}).hint({$natural: 1})),
              0.2);

    // The COLLSCAN stage alone is more expensive than just the IXSCAN stage alone (without the
    // FETCH).
    assert.gt(rootStageCost(coll.find().hint({$natural: 1})),
              ixscanCost({predicate: {}, hint: {a: 1}}));

    // The complete COLLSCAN plan is less expensive than the complete IXCAN plan.
    assert.lt(planCost(coll.find().hint({$natural: 1})), planCost(coll.find().hint({a: 1})));

    /*
     * Cost of IXSCAN
     */

    // Some cost assertions require actual, non-heuristic cardinality estimates
    if (planRankerMode !== "heuristicCE") {
        // IXSCAN cost for an interval containing no documents should be small.
        assert.close(ixscanCost({predicate: {a: {$lt: 0}}, hint: {a: 1}}), 0.000009);

        // IXSCAN cost for an interval containing one document should be small.
        assert.between(0.01, ixscanCost({predicate: {a: {$lte: 0}}, hint: {a: 1}}), 0.02);

        // IXSCAN cost for an interval containing all documents should be the
        // same as IXSCAN over the entire index.
        assert.eq(ixscanCost({predicate: {}, hint: {a: 1}}),
                  ixscanCost({predicate: {a: {$gte: 0}}, hint: {a: 1}}));

        // IXSCAN over 1/2 of the documents should have cost ~ 1/2 of the full IXSCAN.
        assert.between(1.8,
                       ixscanCost({predicate: {}, hint: {a: 1}}) /
                           ixscanCost({predicate: {a: {$gte: coll.count() / 2}}, hint: {a: 1}}),
                       2.2);

        // IXSCAN a wider interval should have greater cost.
        assert.lt(ixscanCost({predicate: {a: {$lt: 100}}, hint: {a: 1}}),
                  ixscanCost({predicate: {a: {$lt: 1000}}, hint: {a: 1}}));

        // IXSCAN of same number of keys over one or more than one interval should have similar
        // cost.
        assert.close(
            ixscanCost({predicate: {a: {$lte: 3}}, hint: {a: 1}}),
            ixscanCost({predicate: {$or: [{a: 0}, {a: 1}, {a: 2}, {a: 3}]}, hint: {a: 1}}));

        // IXSCAN over $or should have similar cost as an IXSCAN over an equivalent $in.
        assert.close(ixscanCost({predicate: {$or: [{a: 0}, {a: 1}, {a: 2}, {a: 3}]}, hint: {a: 1}}),
                     ixscanCost({predicate: {a: {$in: [0, 1, 2, 3]}}, hint: {a: 1}}));
    }

    // IXSCAN with equivalent predicates should have the same cost.
    assert.eq(ixscanCost({predicate: {a: {$lt: 1}}, hint: {a: 1}}),
              ixscanCost({predicate: {a: {$lt: 1, $gte: 0}}, hint: {a: 1}}));

    // TODO(SERVER-98102): IXSCAN cost for two distinct indexes is currently identical
    assert.eq(ixscanCost({predicate: {a: {$lt: 500}}, hint: {a: 1}}),
              ixscanCost({predicate: {a: {$lt: 500}}, hint: {a: 1, b: 1}}));

    // IXSCAN over a multikey index should be more expensive than an IXSCAN over a non-multikey
    // index.
    // TODO(SERVER-100828): assert.lt(ixscanCost({predicate: {a: {$lt: 500}}, hint: {a: 1}}),
    //           ixscanCost({predicate: {a_multikey: {$lt: 500}}, hint: {a_multikey: 1}}));

    // TODO(SERVER-100828): assert.lt(ixscanCost({predicate: {a: 1}, hint: {a: 1}}),
    //          ixscanCost({predicate: {a_multikey: 1}, hint: {a_multikey: 1}}));

    // IXSCAN with a single seek should have a lower cost than an index scan with skips.
    const predicateOverAandB = {a: 5, b: {$gt: 6}};
    assert.lt(ixscanCost({predicate: predicateOverAandB, hint: {a: 1, b: 1}}),
              ixscanCost({predicate: predicateOverAandB, hint: {b: 1, a: 1}}));

    // IXSCAN over an index that matches all predicates should produce a lower-cost
    // plan than any of the alternatives.
    assert.lt(planCost(coll.find(predicateOverAandB).hint({a: 1, b: 1})),
              planCost(coll.find(predicateOverAandB).hint({a: 1})));
    assert.lt(planCost(coll.find(predicateOverAandB).hint({a: 1, b: 1})),
              planCost(coll.find(predicateOverAandB).hint({b: 1})));

    /*
     * Cost of FETCH
     */

    if (planRankerMode !== "heuristicCE") {
        // FETCH cost should be very small if there is nothing to fetch.
        assert.lt(rootStageCost(coll.find({a: -1}).hint({a: 1})), 0.01);

        // FETCH cost should be small if there is one document to fetch.
        assert.lt(rootStageCost(coll.find({a: 1}).hint({a: 1})), 0.01);

        // FETCH cost should be somewhat roughly proportional to the number of input rows.
        assert.between(9,
                       rootStageCost(coll.find({a: {$lt: coll.count() / 10}}).hint({a: 1})) /
                           rootStageCost(coll.find({a: {$lt: coll.count() / 100}}).hint({a: 1})),
                       10);
    }

    // FETCH cost should be the approximately the same with or without a residual predicate.
    assert.lt(rootStageCost(coll.find({a: 1, b: 1}).hint({a: 1})) -
                  rootStageCost(coll.find({a: 1}).hint({a: 1})),
              0.01);

    // FETCH cost should depend on the number of residual predicates.
    assert.lt(rootStageCost(coll.find({a: 1, b: 1}).hint({a: 1})),
              rootStageCost(coll.find({a: 1, b: 1, c: 1, d: 1}).hint({a: 1})));

    // FETCH cost should be the same regardless of the selectivity of the residual predicate.
    assert.eq(rootStageCost(coll.find({b: 0}).hint({a: 1})),
              rootStageCost(coll.find({b: 1}).hint({a: 1})));

    /*
     * Cost of SORT
     */

    // Sorting 1 row has neligible cost.
    assert.lt(rootStageCost(collOneRow.find().sort({a: 1}).hint({$natural: 1})), 0.01);

    // Sorting 1 row has neligible cost.
    if (planRankerMode !== "heuristicCE") {
        assert.lt(rootStageCost(coll.find({a: 1}).sort({a: 1}).hint({$natural: 1})), 0.01);

        // Sorting 1000 rows has a greater cost than sorting 100, but not 100x.
        assert.lt(rootStageCost(coll.find({a: {$lt: 1000}}).sort({a: 1}).hint({$natural: 1})) /
                      rootStageCost(coll.find({a: {$lt: 100}}).sort({a: 1}).hint({$natural: 1})),
                  15);
    }

    // SORT over COLLSCAN and over IXSCAN should have the same cost.
    assert.close(rootStageCost(coll.find().sort({b: 1}).hint({$natural: 1})),
                 rootStageCost(coll.find().sort({b: 1}).hint({a: 1})));

    /*
     * Cost of SORT + $limit
     */

    // SORT with a small $limit should have a small cost
    assert.lt(rootStageCost(coll.find().sort({a: 1}).limit(1).hint({$natural: 1})), 0.0002);
    // TODO(SERVER-100648): assert.lt(rootStageCost(coll.find().sort({a:1}).limit(2).hint({$natural:
    // 1})), 0.001);

    // SORT with a large $limit should have the full cost
    // TODO(SERVER-100650):
    // assert.eq(rootStageCost(coll.find().sort({a:1}).limit(1000000).hint({$natural: 1})),
    // rootStageCost(coll.find().sort({a:1}).hint({$natural: 1})));

    /*
     * Cost of stand-alone LIMIT
     */

    // LIMIT cost should not depend on the size of the collection.
    // TODO(SERVER-100647): assert.eq(rootStageCost(coll.find().limit(1).hint({$natural: 1}),
    // false), rootStageCost(collOneRow.find().limit(1).hint({$natural: 1}), false));
    // TODO(SERVER-100647): IN DOC assert.eq(rootStageCost(coll.find().limit(1).hint({a: 1}),
    // false), rootStageCost(collOneRow.find().limit(1).hint({a: 1}),false));

    // LIMIT cost should depend on the limit itself
    // TODO(SERVER-100647): assert.lt(rootStageCost(coll.find().limit(1).hint({$natural: 1})),
    // rootStageCost(collOneRow.find().limit(10).hint({$natural: 1})));
    // TODO(SERVER-100647): assert.lt(rootStageCost(coll.find().limit(1).hint({a: 1})),
    // rootStageCost(collOneRow.find().limit(10).hint({a: 1})));

    // LIMIT cost should not depend on the type of input (COLLSCAN vs. FETCH + IXSCAN)
    // TODO(SERVER-100647): assert.eq(rootStageCost(coll.find().limit(100).hint({$natural: 1})),
    // rootStageCost(collOneRow.find().limit(100).hint({a: 1})));

    // LIMIT 0 is equivalent to no limit and should be optimized away to not appear in the plan at
    // all
    assert.eq(planCost(coll.find().hint({$natural: 1})),
              planCost(coll.find().limit(0).hint({$natural: 1})));
    assert.eq(planCost(coll.find().hint({a: 1})), planCost(coll.find().limit(0).hint({a: 1})));

    /*
     * Cost of stand-alone SKIP
     */

    // Large SKIP should be equivalent to reading the entire input
    // TODO(SERVER-100651): assert.eq(planCost(coll.find().skip(10000000).hint({$natural: 1})),
    // planCost(coll.find().hint({$natural: 1})));
    // TODO(SERVER-100651): assert.eq(planCost(coll.find().skip(10000000).hint({a: 1})),
    // planCost(coll.find().hint({a: 1})));

    // skip(0) is equivalent to no skip() and should be optimized away to not appear in the plan at
    // all
    assert.eq(planCost(coll.find().hint({$natural: 1})),
              planCost(coll.find().skip(0).hint({$natural: 1})));
    assert.eq(planCost(coll.find().hint({a: 1})), planCost(coll.find().skip(0).hint({a: 1})));

    /*
     * Cost of PROJECTION_COVERED and PROJECTION_SIMPLE
     */

    // Projections increase the cost of the plan.
    assert.gt(planCost(coll.find({}, {_id: 0}).hint({$natural: 1})),
              planCost(coll.find().hint({$natural: 1})));
    assert.gt(planCost(coll.find({}, {_id: 0}).hint({a: 1})), planCost(coll.find().hint({a: 1})));

    // Projections over empty imputs have negligible cost.
    assert.lt(rootStageCost(collZeroRows.find({}, {_id: 0}).hint({$natural: 1})), 0.01);
    assert.lt(rootStageCost(collZeroRows.find({}, {_id: 0}).hint({a: 1})), 0.01);

    // Projections over more documents are more costly.
    assert.gt(rootStageCost(coll.find({}, {_id: 0}).hint({$natural: 1})),
              rootStageCost(collOneRow.find().hint({$natural: 1})));
    assert.gt(rootStageCost(coll.find({}, {_id: 0}).hint({a: 1})),
              rootStageCost(collOneRow.find().hint({a: 1})));

    // More projections are more costly.
    assert.gt(rootStageCost(coll.find({}, {a: 1, b: 1, c: 1}).hint({$natural: 1})),
              rootStageCost(collOneRow.find({}, {a: 1}).hint({$natural: 1})));
}

for (const planRankerMode of ["samplingCE", "histogramCE", "heuristicCE"]) {
    try {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}));
        assert.commandWorked(db.adminCommand({setParameter: 1, samplingConfidenceInterval: "99"}));
        assert.commandWorked(db.adminCommand({setParameter: 1, samplingMarginOfError: 1}));

        runTest(planRankerMode);
    } finally {
        // Make sure that we restore the defaults no matter what
        assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: false}));
        assert.commandWorked(db.adminCommand({setParameter: 1, samplingConfidenceInterval: "95"}));
        assert.commandWorked(db.adminCommand({setParameter: 1, samplingMarginOfError: 5}));
    }
}
