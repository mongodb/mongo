/**
 * Test that the join cache entry is invalidated (or not) depending on any DDLs that were issued.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

function joinPlanCacheStats(db) {
    const planCache = db.serverStatus().metrics.query.planCache;
    assert(planCache.hasOwnProperty("join"), "missing metrics.query.planCache.join", {
        planCache,
    });
    const join = planCache.join;
    assert(join.hasOwnProperty("hits"), "missing join.hits", {join});
    assert(join.hasOwnProperty("misses"), "missing join.misses", {join});

    return {hits: join.hits, misses: join.misses};
}

function runCommandAndCheckCache(command) {
    const {hits: beforeHits, misses: beforeMisses} = joinPlanCacheStats(db);

    assert.commandWorked(db.runCommand(command));
    assert.commandWorked(db.runCommand(command));

    const {hits: afterHits, misses: afterMisses} = joinPlanCacheStats(db);
    const hits = afterHits - beforeHits;
    const misses = afterMisses - beforeMisses;

    return {hits, misses};
}

function checkCacheInvalidation({case: testCaseName, adminDDLs, ddls, baseObject = "base_coll"}) {
    print();
    print();
    print(`### ${testCaseName}`);

    const ddlsForOutput = [];
    if (adminDDLs) ddlsForOutput.push(...adminDDLs);
    if (ddls) ddlsForOutput.push(...ddls);
    print("```json");
    printjson(ddlsForOutput);
    print("```");

    assert.commandWorked(db.dropDatabase());

    for (const collection of ["base_coll", "lookup_coll", "unrelated"]) {
        assert.commandWorked(db[collection].createIndex({a: 1}));
        assert.commandWorked(db[collection].createIndex({b: 1}));
        assert.commandWorked(db[collection].createIndex({unrelated_field: 1}));
        assert.commandWorked(db[collection].createIndex({single_table_predicate: 1}));
        assert.commandWorked(db[collection].createIndex({single_table_view_predicate: 1}));

        assert.commandWorked(db[collection].createIndex({hidden_idx: 1}));
        assert.commandWorked(db[collection].hideIndex({hidden_idx: 1}));
        assert.commandWorked(
            db[collection].createIndex({single_table_predicate: 1, hidden_idx: 1}),
        );
        assert.commandWorked(db[collection].hideIndex({single_table_predicate: 1, hidden_idx: 1}));

        assert.commandWorked(db[collection].insertOne({a: 1, b: 1}));
    }

    assert.commandWorked(db.createView("base_v", "base_coll", []));
    assert.commandWorked(
        db.createView("base_v_extra_predicate", "base_coll", [
            {$match: {single_table_view_predicate: 1}},
        ]),
    );

    const command = {
        "aggregate": baseObject,
        "pipeline": [
            {
                $lookup: {
                    from: "lookup_coll",
                    localField: "a",
                    foreignField: "a",
                    as: "lookup_coll",
                    pipeline: [{$match: {single_table_predicate: 1}}],
                },
            },
            {$unwind: "$lookup_coll"},
            {$match: {single_table_predicate: 1}},
        ],
        cursor: {},
    };

    const {hits: hitsBeforeDDL, misses: missesBeforeDDL} = runCommandAndCheckCache(command);
    // Expect that the cache was hit on the second execution
    assert.gt(hitsBeforeDDL, 0, "expected a cache hit after first execution");
    assert.lt(missesBeforeDDL, 2, "expected no more than 1 miss after first execution");

    if (adminDDLs) {
        for (const adminDDL of adminDDLs) {
            assert.commandWorked(db.adminCommand(adminDDL));
        }
    }

    if (ddls) {
        for (const ddl of ddls) {
            assert.commandWorked(db.runCommand(ddl));
        }
    }

    const {hits: hitsAfterDDL} = runCommandAndCheckCache(command);

    return hitsAfterDDL;
}

function assertCacheInvalidated(test) {
    const hitsAfterDDL = checkCacheInvalidation(test);
    switch (hitsAfterDDL) {
        case 0:
            // Cache did not kick in at all
            print("> [!INFO]");
            print(`> Cache did not kick in at all`);
            break;
        case 1:
            print("> [!INFO]");
            print(`> As expected, the cache entry was correctly invalidated.`);
            break;
        case 2:
            print("> [!WARNING]");
            print(`> Cache entry was expected to be invalidated but was not!`);
    }
}

function assertCacheNotInvalidated(test) {
    const hitsAfterDDL = checkCacheInvalidation(test);
    switch (hitsAfterDDL) {
        case 0:
            // Cache did not kick in at all
            print("> [!WARNING]");
            print(`> Cache did not kick in at all`);
            break;
        case 1:
            print("> [!WARNING]");
            print(`> Cache entry was expected to NOT be invalidated but it was!`);
            break;
        case 2:
            print("> [!INFO]");
            print(`> As expected, the cache entry was correctly NOT invalidated.`);
            break;
    }
}

const dbName = db.getName();

const cacheInvalidationNotExpected = [
    {
        case: "No DDL",
        ddls: [],
    },

    {
        case: "Normal insert",
        ddls: [
            {
                "insert": "base_coll",
                "documents": [{a: 10}],
            },
        ],
    },

    {
        case: "Creating an unrelated collection",
        ddls: [
            {
                "create": "baz",
            },
        ],
    },

    {
        case: "Creating an index on an unrelated collection",
        ddls: [
            {
                "createIndexes": "unrelated",
                "indexes": [
                    {
                        "name": "d_1",
                        "key": {d: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Creating a new unrelated index on the base collection",
        ddls: [
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "d_1",
                        "key": {d: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Creating a new unrelated index on the lookup collection",
        ddls: [
            {
                "createIndexes": "lookup_coll",
                "indexes": [
                    {
                        "name": "d_1",
                        "key": {d: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Unhiding an unrelated index on the base collection",
        ddls: [
            {
                collMod: "base_coll",
                index: {
                    name: "hidden_idx_1",
                    hidden: false,
                },
            },
        ],
    },

    {
        case: "Unhiding an unrelated index on the lookup collection",
        ddls: [
            {
                collMod: "lookup_coll",
                index: {
                    name: "hidden_idx_1",
                    hidden: false,
                },
            },
        ],
    },

    {
        case: "Dropping an unrelated collection",
        ddls: [
            {
                "drop": "unrelated",
            },
        ],
    },

    {
        case: "Dropping an unrelated index on the base collection",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "unrelated_field_1",
            },
        ],
    },

    {
        case: "Dropping an unrelated index on the join collection",
        ddls: [
            {
                "dropIndexes": "lookup_coll",
                "index": "unrelated_field_1",
            },
        ],
    },

    {
        case: "Dropping an unrelated index on the view's base collection",
        baseObject: "base_v",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "unrelated_field_1",
            },
        ],
    },

    {
        case: "Dropping and recreating an index using exactly the same definition",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: 1},
                    },
                ],
            },
        ],
    },
];

const cacheInvalidationExpected = [
    {
        case: "Dropping the database",
        ddls: [
            {
                "dropDatabase": 1,
            },
        ],
    },
    {
        case: "Dropping the base collection (without recreating it)",
        ddls: [
            {
                "drop": "base_coll",
            },
        ],
    },

    {
        case: "Dropping the $lookup collection (without recreating it)",
        ddls: [
            {
                "drop": "lookup_coll",
            },
        ],
    },
    {
        case: "Dropping and recreating the base collection",
        ddls: [
            {
                "drop": "base_coll",
            },
            {
                "create": "base_coll",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating the lookup collection",
        ddls: [
            {
                "drop": "lookup_coll",
            },
            {
                "create": "lookup_coll",
            },
            {
                "createIndexes": "lookup_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Renaming the base collection",
        adminDDLs: [
            {
                "renameCollection": `${dbName}.base_coll`,
                "to": `${dbName}.base_coll_renamed`,
            },
        ],
    },

    {
        case: "Renaming the base collection and creating a new one with the same name",
        adminDDLs: [
            {
                "renameCollection": `${dbName}.base_coll`,
                "to": `${dbName}.base_coll_renamed`,
            },
        ],
        ddls: [
            {
                "create": `base_coll`,
            },
            {
                "createIndexes": `base_coll`,
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping the join index on the base collection",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
        ],
    },

    {
        case: "Dropping the join index on the lookup collection",
        ddls: [
            {
                "dropIndexes": "lookup_coll",
                "index": "a_1",
            },
        ],
    },

    {
        case: "Dropping the index on the base collection's single-table predicate",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "single_table_predicate_1",
            },
        ],
    },

    {
        case: "Dropping the index on the $lookup collection's single-table predicate",
        ddls: [
            {
                "dropIndexes": "lookup_coll",
                "index": "single_table_predicate_1",
            },
        ],
    },

    {
        case: "Hiding the join index on the base collection",
        ddls: [
            {
                collMod: "base_coll",
                index: {
                    name: "a_1",
                    hidden: true,
                },
            },
        ],
    },

    {
        case: "Hiding the join index on the lookup collection",
        ddls: [
            {
                collMod: "lookup_coll",
                index: {
                    name: "a_1",
                    hidden: true,
                },
            },
        ],
    },

    {
        case: "TODO(SERVER-134231): Hiding the index on the base collection's single-table predicate",
        ddls: [
            {
                collMod: "base_coll",
                index: {
                    name: "single_table_predicate_1",
                    hidden: true,
                },
            },
        ],
    },

    {
        case: "TODO(SERVER-134231) Hiding the index on the $lookup collection's single-table predicate",
        ddls: [
            {
                collMod: "lookup_coll",
                index: {
                    name: "single_table_predicate_1",
                    hidden: true,
                },
            },
        ],
    },

    {
        case: "Unhiding a potentially useful index on the base collection",
        ddls: [
            {
                collMod: "base_coll",
                index: {
                    name: "single_table_predicate_1_hidden_idx_1",
                    hidden: false,
                },
            },
        ],
    },

    {
        case: "Unhiding a potentially useful index on the lookup collection",
        ddls: [
            {
                collMod: "lookup_coll",
                index: {
                    name: "single_table_predicate_1_hidden_idx_1",
                    hidden: false,
                },
            },
        ],
    },

    {
        case: "Creating a new index potentially useful for joins on the base collection",
        ddls: [
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1_b_1",
                        "key": {a: 1, b: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Creating a new index potentially useful for joins on the lookup collection",
        ddls: [
            {
                "createIndexes": "lookup_coll",
                "indexes": [
                    {
                        "name": "a_1_b_1",
                        "key": {a: 1, b: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Creating a new index potentially useful for single-table predicate on the base collection",
        ddls: [
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "single_table_predicate_1_x_1",
                        "key": {single_table_predicate: 1, x: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Creating a new index potentially useful for single-table predicate on the lookup collection",
        ddls: [
            {
                "createIndexes": "lookup_coll",
                "indexes": [
                    {
                        "name": "single_table_predicate_1_x_1",
                        "key": {single_table_predicate: 1, x: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Creating a new index potentially useful for the single-table predicate on the base view",
        baseObject: "base_v_extra_predicate",
        ddls: [
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "single_table_view_predicate_1_x_1",
                        "key": {single_table_view_predicate: 1, x: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Making the join index multikey in the base collection",
        ddls: [
            {
                "insert": "base_coll",
                "documents": [{a: [1, 2]}],
            },
        ],
    },

    {
        case: "Making the join index multikey in the lookup collection",
        ddls: [
            {
                "insert": "lookup_coll",
                "documents": [{a: [1, 2]}],
            },
        ],
    },

    {
        case: "TODO(SERVER-130790): Making the single-table predicate index multikey",
        ddls: [
            {
                "insert": "base_coll",
                "documents": [{single_table_predicate: [1, 2]}],
            },
            {
                "insert": "lookup_coll",
                "documents": [{single_table_predicate: [1, 2]}],
            },
        ],
    },

    {
        case: "Making an index unique on the base collection",
        ddls: [
            {
                collMod: "base_coll",
                index: {
                    name: "a_1",
                    prepareUnique: true,
                },
            },
            {
                collMod: "base_coll",
                index: {
                    name: "a_1",
                    unique: true,
                },
            },
        ],
    },

    {
        case: "Making an index unique on the lookup collection",
        ddls: [
            {
                collMod: "lookup_coll",
                index: {
                    name: "a_1",
                    prepareUnique: true,
                },
            },
            {
                collMod: "lookup_coll",
                index: {
                    name: "a_1",
                    unique: true,
                },
            },
        ],
    },

    {
        case: "Changing the definition of the view used as base collection",
        baseObject: "base_v",
        ddls: [
            {
                collMod: "base_v",
                viewOn: "lookup_coll",
                pipeline: [],
            },
        ],
    },

    {
        case: "Dropping the join index on the view's base collection",
        baseObject: "base_v",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
        ],
    },

    {
        case: "Dropping the index on the view's base collection's single-table predicate",
        baseObject: "base_v",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "single_table_predicate_1",
            },
        ],
    },

    {
        case: "Dropping the index on the view's single-table predicate",
        baseObject: "base_v_extra_predicate",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "single_table_view_predicate_1",
            },
        ],
    },

    {
        case: "Dropping and recreating an index under a different name",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_new_1",
                        "key": {a: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating an index with a different keyPattern (different field)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {z: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating an index with a different keyPattern (different field list)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: 1, b: 1},
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating an index with a different keyPattern (different index direction)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: -1},
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating an index with a different keyPattern (different uniqueness)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: -1},
                        unique: true,
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating an index with a different keyPattern (different partialFilterExpression)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: -1},
                        partialFilterExpression: {a: 0},
                    },
                ],
            },
        ],
    },

    {
        case: "TODO(SERVER-134231) Dropping and recreating an index with a different keyPattern (different sparseness)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: -1},
                        sparse: true,
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating an index with a different keyPattern (different hidden state)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: -1},
                        hidden: true,
                    },
                ],
            },
        ],
    },

    {
        case: "TODO(SERVER-134231): Dropping and recreating an index with a different keyPattern (different collation)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: -1},
                        "collation": {locale: "fr"},
                    },
                ],
            },
        ],
    },

    {
        case: "Dropping and recreating an index with a different keyPattern (different index type)",
        ddls: [
            {
                "dropIndexes": "base_coll",
                "index": "a_1",
            },
            {
                "createIndexes": "base_coll",
                "indexes": [
                    {
                        "name": "a_1",
                        "key": {a: "hashed"},
                    },
                ],
            },
        ],
    },
];

print("## Cache invalidation NOT expected for those scenarios:");
for (const test of cacheInvalidationNotExpected) {
    assertCacheNotInvalidated(test);
}

print("## Cache invalidation expected for those scenarios:");
for (const test of cacheInvalidationExpected) {
    assertCacheInvalidated(test);
}
