/**
 * Tests boolean expression simplifer produces expected results.
 * @tags: [
 * requires_fcv_72,
 * # explain command, used by the test, does not support majority read concern.
 *  assumes_read_concern_local,
 * ]
 */

import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/analyze_plan.js";

const parameterName = "internalQueryEnableBooleanExpressionsSimplifier";
const isSImplifierEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, [parameterName]: 1}))[parameterName];
if (!isSImplifierEnabled) {
    jsTest.log("Skipping the Boolean simplier tests, since the simplifier is disabled...");
    quit();
}

/**
 * Checks possible representations of an empty filter in query plans, which can be empty object '{}'
 * or missing value.
 */
function isEmptyFilter(filter) {
    // null or undefined
    if (filter === null || filter === undefined) {
        return true;
    }
    if (Object.keys(filter).length === 0) {
        return true;
    }
    return false;
}

/**
 * Checks that the given 'filter' expression is simplified to the 'simplifiedFilter' expression in
 * the generated COLLSCAN plan.
 */
function testSimplifierForCollscan(coll, filter, simplifiedFilter) {
    const explain = assert.commandWorked(coll.find(filter).explain());
    const winningPlan = getWinningPlanFromExplain(explain);
    const collScans = getPlanStages(winningPlan, "COLLSCAN");
    assert.neq(0, collScans.length, winningPlan);

    for (const collScan of collScans) {
        const emptyFilters = isEmptyFilter(collScan.filter) && isEmptyFilter(simplifiedFilter);
        if (!emptyFilters) {
            assert.docEq(simplifiedFilter, collScan.filter, collScan);
        }
    }
}

db.coll.drop();
assert.commandWorked(db.coll.insertMany([
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 2, b: 1},
    {a: 2, b: 2},
]));

const testCases = [
    // a == 1 and a != 1 is simplified to AlwaysFalse
    [
        {'$or': [{'$and': [{a: 1}, {a: {'$ne': 1}}]}, {b: 2}]},
        {b: {'$eq': 2}},
    ],
    // $elemMatch expression is not mixed with other expressions
    [
        {$and: [{c: 1}, {c: {$elemMatch: {$ne: 1}}}]},
        {$and: [{c: {$elemMatch: {$not: {$eq: 1}}}}, {c: {$eq: 1}}]},
    ],
    // nested $elemMatch
    [
        {d: {$elemMatch: {e: {$elemMatch: {f: {$elemMatch: {$eq: 11}}}}}}},
        {d: {$elemMatch: {e: {$elemMatch: {f: {$elemMatch: {$eq: 11}}}}}}},
    ],
    // CNF -> DNF: we don't simplify an expression if the resulting one is bigger then the original
    // one
    [
        {
            $and: [
                {$or: [{a: {$eq: 1}}, {b: {$eq: 1}}]},
                {$or: [{c: {$eq: 2}}, {d: {$eq: 2}}]},
            ]
        },
        {
            $and: [
                {$or: [{a: {$eq: 1}}, {b: {$eq: 1}}]},
                {$or: [{c: {$eq: 2}}, {d: {$eq: 2}}]},
            ]
        },
    ],
    // Show that we have achieve optimization originally requested in SERVER-31360.
    [
        {$or: [{}, {}]},
        {},
    ],
    // Show that we have achieve optimization originally requested in SERVER-31360.
    [
        {$or: [{}]},
        {},
    ],
    // Show that we have achieve optimization for the first query originally requested in
    // SERVER-22857.
    [{$and: [{"a": 1}, {"a": 1}]}, {a: {$eq: 1}}],
    // Inside $elemMatch $not is not expanded
    [
        {a: {$elemMatch: {$not: {$gt: 5, $lt: 8}, $gt: 4}}},
        {a: {$elemMatch: {$not: {$gt: 5, $lt: 8}, $gt: 4}}},
    ],
    // TODO SERVER-81788: we don't optimize $elemMatch yet as originally requested in the second
    // example of SERVER-22857.
    [
        {
            $and: [
                {a: {$elemMatch: {$and: [{b: {$eq: 9}}, {id: {$eq: 1}}]}}},
                {a: {$eq: {id: {$eq: 1}}}}
            ]
        },
        {
            $and: [
                {a: {$elemMatch: {$and: [{b: {$eq: 9}}, {id: {$eq: 1}}]}}},
                {a: {$eq: {id: {$eq: 1}}}}
            ]
        },
    ],
    // TODO SERVER-81792: we don't optimize $exists yet as originally requested in SERVER-35018.
    [
        {a: {$eq: 1, $exists: true}},
        {$and: [{a: {$eq: 1}}, {a: {$exists: true}}]},
    ],
    // TODO SERVER-81792: we don't optimize $ne yet as originally requested in SERVER-35018.
    [
        {a: {$eq: 1, $ne: null}},
        {$and: [{a: {$eq: 1}}, {a: {$not: {$eq: null}}}]},
    ],
    // Simplify off redundant $or.
    [
        {
            '$and': [
                {b: 0},
                {
                    '$or': [
                        {a: 0},
                        {
                            '$and': [
                                {c: {'$in': [34, 45]}},
                                {c: {'$nin': [34, 45]}},
                            ]
                        }
                    ]
                },
            ]
        },
        {$and: [{a: {$eq: 0}}, {b: {$eq: 0}}]}
    ]
];

for (const [filter, simplifiedFilter] of testCases) {
    testSimplifierForCollscan(db.coll, filter, simplifiedFilter);
}
