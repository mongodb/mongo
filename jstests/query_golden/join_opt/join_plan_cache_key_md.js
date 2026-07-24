/**
 * Test that the hash used for the join plan cache is calculated appropriately:
 * - queries that are expected to reuse the same plan should have the same hash key
 * - queries that are not expected to reuse the same plan should be given different keys
 * - queries that are currently not eligible for join optimization but could be in the future
 *   are placed in a separate set so that one will need to decide their caching behavior when they become eligible
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

function getJoinPlanCacheKey(command) {
    const explain = assert.commandWorked(db.runCommand({explain: command}));
    if (explain.hasOwnProperty("queryPlanner")) {
        return explain.queryPlanner.joinPlanCacheKey;
    } else if (explain.hasOwnProperty("stages")) {
        return explain.stages[0]["$cursor"].queryPlanner.joinPlanCacheKey;
    } else {
        return undefined;
    }
}

function printQueriesAndCacheKeys(command1, command2) {
    const hash1 = getJoinPlanCacheKey(command1);
    const hash2 = getJoinPlanCacheKey(command2);
    print(`Command 1: ${JSON.stringify(command1)}  `);
    print(`Command 2: ${JSON.stringify(command2)}  `);
    print(`Hash 1: ${hash1}  `);
    print(`Hash 2: ${hash2}  `);
    return {hash1, hash2};
}

function assertIdenticalKeys(command1, command2) {
    const {hash1, hash2} = printQueriesAndCacheKeys(command1, command2);
    assert(
        hash1 !== undefined && hash2 !== undefined,
        "Join plan cache keys are undefined for command: " +
            JSON.stringify(command1) +
            " or " +
            JSON.stringify(command2),
    );
    if (hash1 !== hash2) {
        print("> [!WARNING]");
        print(`> Hash keys were expected to be identical but were not!`);
    }
}

function assertDifferentKeys(command1, command2) {
    const {hash1, hash2} = printQueriesAndCacheKeys(command1, command2);
    if (hash1 === hash2) {
        print("> [!WARNING]");
        print(`> Hash keys were expected to be different but were not!`);
    }
}

function assertNoHashKey(command1, command2) {
    const {hash1, hash2} = printQueriesAndCacheKeys(command1, command2);
    if (hash1 !== undefined || hash2 !== undefined) {
        print("> [!WARNING]");
        print(
            `> Hash keys were expected to be undefined but were not! Please move the query to the appropriate section of the test.`,
        );
    }
}

db.dropDatabase();

for (const collection of ["foo", "foo2", "bar", "bar2"]) {
    assert.commandWorked(db[collection].createIndex({a: 1}));
    assert.commandWorked(db[collection].createIndex({b: 1}));
    assert.commandWorked(db[collection].insertOne({a: 1, b: 1}));
}

/*
 * The query pairs below are expected to have identical join plan cache keys so that
 * the join plan is potentially reused across both.
 */
const identicalHashesExpected = [
    {
        case: "Identical keys for completely identical queries",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },
    {
        case: "Identical keys for queries with different suffixes",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$sort: {"a": 1}},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$sort: {"b": 1}},
            ],
        },
    },
    {
        case: "Identical keys for queries with different literals in prefix $match",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"a": 1}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"a": 2}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Identical keys for queries with different literals in suffix $match",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$match: {"a": 1}},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$match: {"a": 2}},
            ],
        },
    },
    {
        case: "Identical keys for queries with different literals in subpipeline $match",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"a": 1}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"a": 2}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Identical keys for queries that are semantically equivalent after predicate pushdown",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"a": 1}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {"$match": {"a": 1}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Identical keys for queries with different $in lists",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"a": {"$in": [1, 2]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"a": {"$in": [2, 3]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Identical keys for queries with different $in list lengths",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"a": {"$in": [1, 2]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"a": {"$in": [1, 2, 3]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Identical keys for queries with $expr in subpipeline with no outer references",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"$expr": {"$eq": ["$a", 1]}}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"$expr": {"$eq": ["$a", 2]}}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Identical keys for queries with let argument to aggregate() used in $expr",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"$expr": {"$eq": ["$a", "$$var"]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            let: {var: 1},
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"$expr": {"$eq": ["$a", "$$var"]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            let: {var: 2},
        },
    },

    {
        case: "Identical keys for queries with variables in the let argument to aggregate() that are named differently",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"$expr": {"$eq": ["$a", "$$var1"]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            let: {var1: 1},
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"$expr": {"$eq": ["$a", "$$var2"]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            let: {var2: 2},
        },
    },
];

/*
 * The query pairs below are expected to have different join plan cache keys because the
 * two queries differ in a manner that is material and the same join plan should
 * not be reused across both.
 */
const differentHashesExpected = [
    {
        case: "Different keys for different base collections",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo2",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Different keys for different join collections",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar2",
                        localField: "a",
                        foreignField: "a",
                        as: "bar2",
                    },
                },
                {$unwind: "$bar2"},
            ],
        },
    },

    {
        case: "Different keys for different predicates in prefix $match",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"a": 1}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"b": 1}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Different keys for different predicates in suffix $match",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$match: {"a": 1}},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$match: {"b": 1}},
            ],
        },
    },

    {
        case: "Different keys for different predicates in subpipeline $match",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"a": 1}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"b": 1}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Different keys for different localField",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "b",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Different keys for different foreignField",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "b",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Different keys for different as in $lookup",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar1",
                    },
                },
                {$unwind: "$bar1"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar2",
                    },
                },
                {$unwind: "$bar2"},
            ],
        },
    },

    {
        case: "Different keys for queries that are not semantically equivalent after predicate pushdown",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"a": 1}}],
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {"$match": {"a": {$gt: 1}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Different keys for queries with different $project plan suffixes",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$project: {"a": 1}},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$project: {"b": 1}},
            ],
        },
    },

    {
        case: "TODO(SERVER-121078): Different keys with $limit and no $limit",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
                {$limit: 1},
            ],
        },
    },

    {
        case: "TODO(SERVER-131472): Different keys with allowDiskUse: true and false",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            allowDiskUse: true,
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            allowDiskUse: false,
        },
    },

    {
        case: "Different keys for queries with different fields in $expr",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"$expr": {"$eq": ["$a", "$$var"]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            let: {var: 1},
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {$match: {"$expr": {"$eq": ["$b", "$$var"]}}},
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            let: {var: 1},
        },
    },

    {
        case: "Different keys for queries with $unwind with preserveNullAndEmptyArrays true/false",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: {path: "$bar", preserveNullAndEmptyArrays: true}},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: {path: "$bar", preserveNullAndEmptyArrays: false}},
            ],
        },
    },

    {
        case: "TODO(SERVER-131752) Different keys expected for queries with and without $_internalJoinHint",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $_internalJoinHint: {
                        perSubsetLevelMode: [{level: NumberInt(0), mode: "ALL"}],
                    },
                },
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "TODO(SERVER-131752) Different keys expected for queries with different $_internalJoinHint",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $_internalJoinHint: {
                        perSubsetLevelMode: [
                            {level: NumberInt(0), hint: {method: "HJ"}, mode: "ALL"},
                        ],
                    },
                },
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $_internalJoinHint: {
                        perSubsetLevelMode: [
                            {level: NumberInt(0), hint: {method: "INLJ"}, mode: "ALL"},
                        ],
                    },
                },
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },
];

/*
 * The pipelines below are not currently eligible for join optimization. But if they ever become eligible in the
 * future, this test will fail so that one can decide if the join plan cache keys should be the same or different
 * for the particular scenario.
 */
const currentlyNoHash = [
    {
        case: "TODO(SERVER-115652): Currently no hashes for queries with let argument to $lookup",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"$expr": {"$eq": ["$a", "$$var"]}}}],
                        let: {var: 2},
                    },
                },
                {$unwind: "$bar"},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                        pipeline: [{$match: {"$expr": {"$eq": ["$a", "$$var"]}}}],
                        let: {var: 3},
                    },
                },
                {$unwind: "$bar"},
            ],
        },
    },

    {
        case: "Currently no hashes for queries with aggregate() with collation",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            collation: {locale: "en_US", strength: 1},
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            collation: {locale: "en_US", strength: 2},
        },
    },

    {
        case: "Currently no hashes for queries with $unwind with includeArrayIndex",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: {path: "$bar", includeArrayIndex: "arrayIndex"}},
            ],
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: {path: "$bar", includeArrayIndex: "arrayIndex2"}},
            ],
        },
    },

    {
        case: "Currently no hashes for queries with aggregate()-level hints",
        command1: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            hint: {"a": 1},
        },
        command2: {
            "aggregate": "foo",
            "pipeline": [
                {
                    $lookup: {
                        from: "bar",
                        localField: "a",
                        foreignField: "a",
                        as: "bar",
                    },
                },
                {$unwind: "$bar"},
            ],
            hint: {"b": 1},
        },
    },
];

print("## Queries where identical join plan cache keys are expected");
for (const test of identicalHashesExpected) {
    print(`### ${test.case}`);
    assertIdenticalKeys(test.command1, test.command2);
}

print("## Queries where different join plan cache keys are expected");
for (const test of differentHashesExpected) {
    print(`### ${test.case}`);
    assertDifferentKeys(test.command1, test.command2);
}

print("## Queries that currently do not have a join plan cache key");
for (const test of currentlyNoHash) {
    print(`### ${test.case}`);
    assertNoHashKey(test.command1, test.command2);
}
