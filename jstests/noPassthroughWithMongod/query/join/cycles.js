/**
 * Tests that join optimization correctly handles cycles in the join graph.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {runTestWithUnorderedComparison} from "jstests/libs/query/join_utils.js";

db.dropDatabase();

//
// Some basic, simple data.
//
const collA = db[jsTestName() + "_a"];
const collB = db[jsTestName() + "_b"];
const collC = db[jsTestName() + "_c"];
const collD = db[jsTestName() + "_d"];

assert.commandWorked(
    collA.insertMany([
        {_id: 0, joinField1: 0, joinField2: 0, joinField3: 0},
        {_id: 1, joinField1: 1, joinField2: 1, joinField3: 1},
    ]),
);
assert.commandWorked(
    collB.insertMany([
        {_id: 0, joinField1: 0, joinField2: 0},
        {_id: 1, joinField1: 1, joinField2: 1},
    ]),
);
assert.commandWorked(
    collC.insertMany([
        {_id: 0, joinField1: 0, joinField2: 1, joinField3: 0},
        {_id: 1, joinField1: 1, joinField2: 0, joinField3: 0},
    ]),
);
assert.commandWorked(
    collD.insertMany([
        {_id: 0, joinField1: 0, joinField2: 0},
        {_id: 1, joinField1: 1, joinField2: 1},
    ]),
);

//
// Some data to mimic TPC-H.
//
const customerColl = db["customer"];
const ordersColl = db["orders"];
const lineitemColl = db["lineitem"];
const supplierColl = db["supplier"];
const nationColl = db["nation"];
const regionColl = db["region"];

assert.commandWorked(
    customerColl.insertMany([
        {c_custkey: 1, c_nationkey: 1},
        {c_custkey: 2, c_nationkey: 2},
        {c_custkey: 3, c_nationkey: 1},
    ]),
);
assert.commandWorked(
    ordersColl.insertMany([
        {o_orderkey: 1, o_custkey: 1},
        {o_orderkey: 2, o_custkey: 2},
        {o_orderkey: 3, o_custkey: 1},
    ]),
);
assert.commandWorked(
    lineitemColl.insertMany([
        {l_orderkey: 1, l_suppkey: 1},
        {l_orderkey: 1, l_suppkey: 2},
        {l_orderkey: 2, l_suppkey: 1},
        {l_orderkey: 2, l_suppkey: 2},
        {l_orderkey: 3, l_suppkey: 2},
    ]),
);
assert.commandWorked(
    supplierColl.insertMany([
        {s_suppkey: 1, s_nationkey: 1},
        {s_suppkey: 2, s_nationkey: 2},
        {s_suppkey: 3, s_nationkey: 1},
    ]),
);
assert.commandWorked(
    nationColl.insertMany([
        {n_nationkey: 1, n_regionkey: 1},
        {n_nationkey: 2, n_regionkey: 2},
        {n_nationkey: 3, n_regionkey: 1},
    ]),
);
assert.commandWorked(regionColl.insertMany([{r_regionkey: 1}, {r_regionkey: 2}]));

// TODO SERVER-111798: Update assertions in this test file once join opt supports true cycles and
// pseudo-cycles.

function runAllTestCases(additionalJoinParams = {}) {
    // Query with implicit cycle of length 3, specified with join predicates A -- B, B -- C on the same
    // field (joinField1). The inferred cycle is A -- B -- C.
    runTestWithUnorderedComparison({
        description: "Implicit cycle of length 3 on joinField1",
        coll: collA,
        additionalJoinParams,
        pipeline: [
            {
                $match: {
                    "_id": 0,
                },
            },
            {
                $lookup: {
                    from: collB.getName(),
                    localField: "joinField1",
                    foreignField: "joinField1",
                    as: "b_docs",
                },
            },
            {$unwind: "$b_docs"},
            {
                $lookup: {
                    from: collC.getName(),
                    localField: "b_docs.joinField1",
                    foreignField: "joinField1",
                    as: "c_docs",
                },
            },
            {$unwind: "$c_docs"},
        ],
        expectedResults: [
            {
                _id: 0,
                joinField1: 0,
                joinField2: 0,
                joinField3: 0,
                b_docs: {_id: 0, joinField1: 0, joinField2: 0},
                c_docs: {_id: 0, joinField1: 0, joinField2: 1, joinField3: 0},
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    // Query with implicit cycle of length 4. Similar to above, but with more collections. Also, the
    // cycle inference required is different. Here, the query specified A -- B, A -- C, and A--D,
    // so the largest cycle inferred is A -- B -- C -- D.
    runTestWithUnorderedComparison({
        description: "Implicit cycle of length 4 on joinField1",
        coll: collA,
        additionalJoinParams,
        pipeline: [
            {
                $match: {
                    "_id": 0,
                },
            },
            {
                $lookup: {
                    from: collB.getName(),
                    localField: "joinField1",
                    foreignField: "joinField1",
                    as: "b_docs",
                },
            },
            {$unwind: "$b_docs"},
            {
                $lookup: {
                    from: collC.getName(),
                    localField: "joinField1",
                    foreignField: "joinField1",
                    as: "c_docs",
                },
            },
            {$unwind: "$c_docs"},
            {
                $lookup: {
                    from: collD.getName(),
                    localField: "joinField1",
                    foreignField: "joinField1",
                    as: "d_docs",
                },
            },
            {$unwind: "$d_docs"},
        ],
        expectedResults: [
            {
                _id: 0,
                joinField1: 0,
                joinField2: 0,
                joinField3: 0,
                b_docs: {_id: 0, joinField1: 0, joinField2: 0},
                c_docs: {_id: 0, joinField1: 0, joinField2: 1, joinField3: 0},
                d_docs: {_id: 0, joinField1: 0, joinField2: 0},
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    // Query with explicit cycle, specified with join predicates A -- B, B -- C, C--A on the same field
    // (joinField1). The cycle is A -- B -- C. This time, we use a mix of pipeline syntax and
    // local/foreignField syntax.
    runTestWithUnorderedComparison({
        description: "Explicit cycle on joinField1",
        coll: collA,
        additionalJoinParams,
        pipeline: [
            {
                $match: {
                    "_id": 0,
                },
            },
            {
                $lookup: {
                    from: collB.getName(),
                    localField: "joinField1",
                    foreignField: "joinField1",
                    as: "b_docs",
                },
            },
            {$unwind: "$b_docs"},
            {
                $lookup: {
                    from: collC.getName(),
                    localField: "b_docs.joinField1",
                    foreignField: "joinField1",
                    let: {a_joinField1: "$joinField1"},
                    pipeline: [{$match: {$expr: {$eq: ["$joinField1", "$$a_joinField1"]}}}],
                    as: "c_docs",
                },
            },
            {$unwind: "$c_docs"},
        ],
        expectedResults: [
            {
                _id: 0,
                joinField1: 0,
                joinField2: 0,
                joinField3: 0,
                b_docs: {_id: 0, joinField1: 0, joinField2: 0},
                c_docs: {_id: 0, joinField1: 0, joinField2: 1, joinField3: 0},
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    // Query with a pseudo-cycle. The join predicates are A -- B on joinField1, B -- C on joinField2,
    // and A -- C on joinField3. This does _not_ form a true cycle.
    runTestWithUnorderedComparison({
        description: "Pseudo-cycle",
        coll: collA,
        additionalJoinParams,
        pipeline: [
            {
                $match: {
                    "_id": 0,
                },
            },
            {
                $lookup: {
                    from: collB.getName(),
                    localField: "joinField1",
                    foreignField: "joinField1",
                    as: "b_docs",
                },
            },
            {$unwind: "$b_docs"},
            {
                $lookup: {
                    from: collC.getName(),
                    localField: "b_docs.joinField2",
                    foreignField: "joinField2",
                    let: {a_joinField3: "$joinField3"},
                    pipeline: [{$match: {$expr: {$eq: ["$joinField3", "$$a_joinField3"]}}}],
                    as: "c_docs",
                },
            },
            {$unwind: "$c_docs"},
        ],
        expectedResults: [
            {
                _id: 0,
                joinField1: 0,
                joinField2: 0,
                joinField3: 0,
                b_docs: {_id: 0, joinField1: 0, joinField2: 0},
                c_docs: {_id: 1, joinField1: 1, joinField2: 0, joinField3: 0},
            },
        ],
        expectedUsedJoinOptimization: false,
    });

    // Query with a mix of true cycles and pseudo-cycles. This query is based on TPC-H Q5. Here, the
    // join graph as specified in the query is:
    // R -- N    C -- O -- L
    //       \  /         /
    //        S ---------/
    // Through an inferred edge, there is a true cycle between N -- C -- S on nationkey. There is also
    // a pseudo-cycle between C -- O -- L -- S.
    runTestWithUnorderedComparison({
        description: "Mix of true cycles and pseudo-cycles",
        coll: customerColl,
        additionalJoinParams,
        pipeline: [
            {
                $match: {c_custkey: 1},
            },
            {
                $lookup: {
                    from: ordersColl.getName(),
                    localField: "c_custkey",
                    foreignField: "o_custkey",
                    as: "orders",
                },
            },
            {$unwind: "$orders"},
            {
                $lookup: {
                    from: lineitemColl.getName(),
                    localField: "orders.o_orderkey",
                    foreignField: "l_orderkey",
                    as: "lineitem",
                },
            },
            {$unwind: "$lineitem"},
            {
                $lookup: {
                    from: supplierColl.getName(),
                    localField: "lineitem.l_suppkey",
                    foreignField: "s_suppkey",
                    as: "supplier",
                    let: {c_nationkey: "$c_nationkey"},
                    pipeline: [{$match: {$expr: {$eq: ["$s_nationkey", "$$c_nationkey"]}}}],
                },
            },
            {$unwind: "$supplier"},
            {
                $lookup: {
                    from: nationColl.getName(),
                    localField: "supplier.s_nationkey",
                    foreignField: "n_nationkey",
                    as: "nation",
                },
            },
            {$unwind: "$nation"},
            {
                $lookup: {
                    from: regionColl.getName(),
                    localField: "nation.n_regionkey",
                    foreignField: "r_regionkey",
                    as: "region",
                },
            },
            {$unwind: "$region"},
            {
                $project: {
                    _id: 0,
                    "orders._id": 0,
                    "lineitem._id": 0,
                    "supplier._id": 0,
                    "nation._id": 0,
                    "region._id": 0,
                },
            },
        ],
        expectedResults: [
            {
                c_custkey: 1,
                c_nationkey: 1,
                orders: {o_orderkey: 1, o_custkey: 1},
                lineitem: {l_orderkey: 1, l_suppkey: 1},
                supplier: {s_suppkey: 1, s_nationkey: 1},
                nation: {n_nationkey: 1, n_regionkey: 1},
                region: {r_regionkey: 1},
            },
        ],
        expectedUsedJoinOptimization: false,
    });
}

print(`Running all test cases with bottom up enumeration`);
runAllTestCases({internalJoinReorderMode: "bottomUp"});

print(`Running all test cases with random enumeration and NLJ`);
runAllTestCases({
    internalJoinReorderMode: "random",
    internalRandomJoinOrderSeed: 44,
    internalRandomJoinReorderDefaultToHashJoin: false,
});

print(`Running all test cases with random enumeration and default to HJ`);
runAllTestCases({
    internalJoinReorderMode: "random",
    internalRandomJoinOrderSeed: 44,
    internalRandomJoinReorderDefaultToHashJoin: true,
});

// Construct indexes to test INLJ.
for (const coll of [collA, collB, collC, collD]) {
    for (const field of ["joinField1", "joinField2", "joinField3"]) {
        assert.commandWorked(coll.createIndex({[field]: 1}));
    }
}
assert.commandWorked(customerColl.createIndex({c_custkey: 1}));
assert.commandWorked(customerColl.createIndex({c_nationkey: 1}));
assert.commandWorked(ordersColl.createIndex({o_custkey: 1}));
assert.commandWorked(ordersColl.createIndex({o_orderkey: 1}));
assert.commandWorked(lineitemColl.createIndex({l_orderkey: 1}));
assert.commandWorked(lineitemColl.createIndex({l_suppkey: 1}));
assert.commandWorked(supplierColl.createIndex({s_suppkey: 1}));
assert.commandWorked(supplierColl.createIndex({s_nationkey: 1}));
assert.commandWorked(nationColl.createIndex({n_nationkey: 1}));
assert.commandWorked(nationColl.createIndex({n_regionkey: 1}));
assert.commandWorked(regionColl.createIndex({r_regionkey: 1}));

print(`Running all test cases with bottom up enumeration and indexes`);
runAllTestCases({internalJoinReorderMode: "bottomUp"});

print(`Running all test cases with random enumeration and INLJ`);
runAllTestCases({
    internalJoinReorderMode: "random",
    internalRandomJoinOrderSeed: 44,
    internalRandomJoinReorderDefaultToHashJoin: false,
});
