// Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
// agg stage.
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Creating the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const ns = {
    db: db.getName(),
    coll: coll.getName(),
};
const qsutils = new QuerySettingsUtils(db, coll.getName());
qsutils.removeAllQuerySettings();

/**
 * Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
 * agg stage.
 *
 *  params.queryA - query of some shape
 *  params.queryB - query of shape different from A
 *  params.queryBPrime - different query of the same shape as params.queryB
 *  params.querySettingsA - query setting for all queries of the shape as params.queryA
 *  params.querySettingsB - query setting for all queries of the shape as params.queryB
 */
function testQuerySettingsUsing(params) {
    const queryShapeHashA = qsutils.getQueryShapeHashFromExplain(params.queryA);

    // Ensure that query settings cluster parameter is empty.
    {
        qsutils.assertQueryShapeConfiguration([]);
    }

    // Ensure that 'querySettings' cluster parameter contains QueryShapeConfiguration after
    // invoking setQuerySettings command.
    {
        qsutils.assertExplainQuerySettings(params.queryA, undefined);
        assert.commandWorked(db.adminCommand({setQuerySettings: params.queryA, settings: params.querySettingsA}));
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(params.querySettingsA, params.queryA),
        ]);
    }

    // Ensure that 'querySettings' cluster parameter contains both QueryShapeConfigurations
    // after invoking setQuerySettings command.
    {
        qsutils.assertExplainQuerySettings(params.queryB, undefined);
        assert.commandWorked(db.adminCommand({setQuerySettings: params.queryB, settings: params.querySettingsB}));
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(params.querySettingsA, params.queryA),
            qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryB),
        ]);
    }

    // Ensure that 'querySettings' cluster parameter gets updated on subsequent call of
    // setQuerySettings by passing a QueryShapeHash.
    {
        assert.neq(queryShapeHashA, undefined);
        assert.commandWorked(db.adminCommand({setQuerySettings: queryShapeHashA, settings: params.querySettingsB}));
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryA),
            qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryB),
        ]);
    }

    // Ensure that 'querySettings' cluster parameter gets updated on subsequent call of
    // setQuerySettings by passing a different QueryInstance with the same QueryShape.
    {
        assert.commandWorked(db.adminCommand({setQuerySettings: params.queryBPrime, settings: params.querySettingsA}));
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryA),
            qsutils.makeQueryShapeConfiguration(params.querySettingsA, params.queryBPrime),
        ]);
    }

    // Ensure that removeQuerySettings command removes one query settings from the
    // 'settingsArray' of the 'querySettings' cluster parameter by providing a query instance.
    {
        assert.commandWorked(db.adminCommand({removeQuerySettings: params.queryBPrime}));
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryA),
        ]);
        qsutils.assertExplainQuerySettings(params.queryB, undefined);
    }

    // Ensure that query settings cluster parameter is empty by issuing a removeQuerySettings
    // command providing a query shape hash.
    {
        assert.commandWorked(db.adminCommand({removeQuerySettings: queryShapeHashA}));
        qsutils.assertQueryShapeConfiguration([]);
        qsutils.assertExplainQuerySettings(params.queryA, undefined);
    }

    // Ensure that query settings can also be added for a query shape hash without providing a
    // representative query.
    {
        assert.commandWorked(db.adminCommand({setQuerySettings: queryShapeHashA, settings: params.querySettingsA}));

        // Call 'assertQueryShapeConfiguration()' with shouldRunExplain = false, because we
        // don't have a representative query.
        qsutils.assertQueryShapeConfiguration([{settings: params.querySettingsA}], false /* shouldRunExplain */);
        // Run 'assertExplainQuerySettings()' separately with a query.
        qsutils.assertExplainQuerySettings(params.queryA, params.querySettingsA);

        // Assert there is no 'debugQueryShape'.
        assert(
            !qsutils
                .getQuerySettings({
                    showDebugQueryShape: true,
                })[0]
                .hasOwnProperty("debugQueryShape"),
        );
        assert.commandWorked(db.adminCommand({removeQuerySettings: queryShapeHashA}));
        qsutils.assertQueryShapeConfiguration([]);
    }
}

function testQuerySettingsParameterized({find, distinct, aggregate}) {
    // Testing find query settings.
    const filter = {a: "$$variableA", b: "$$variableB"};
    testQuerySettingsUsing({
        queryA: qsutils.makeFindQueryInstance({
            filter,
            let: {
                variableA: 10,
                variableB: 15,
            },
        }),
        queryB: qsutils.makeFindQueryInstance({
            filter,
            let: {
                variableA: "string",
                variableB: "super-string",
            },
        }),
        queryBPrime: qsutils.makeFindQueryInstance({
            filter,
            let: {
                variableA: "another-string",
                variableB: "still-super-string",
            },
        }),
        ...find,
    });

    // Same for aggregate query settings.
    const pipeline = [
        {
            $match: {
                matchKey: "$$variable",
            },
        },
        {
            $group: {
                _id: "groupID",
                values: {$addToSet: "$value"},
            },
        },
    ];
    testQuerySettingsUsing({
        queryA: qsutils.makeAggregateQueryInstance({
            pipeline,
            let: {
                variable: 10,
            },
        }),
        queryB: qsutils.makeAggregateQueryInstance({
            pipeline,
            let: {
                variable: "string",
            },
        }),
        queryBPrime: qsutils.makeAggregateQueryInstance({
            pipeline,
            let: {
                variable: "another-string",
            },
        }),
        ...aggregate,
    });

    // Same for distinct query settings.
    testQuerySettingsUsing({
        queryA: qsutils.makeDistinctQueryInstance({key: "k", query: {a: 1, b: 2}}),
        queryB: qsutils.makeDistinctQueryInstance({key: "k", query: {b: "string", a: "still-super-string"}}),
        queryBPrime: qsutils.makeDistinctQueryInstance({
            key: "k",
            query: {b: "another string", a: "no-more-super-string"},
        }),
        ...distinct,
    });
}

// Test changing allowed indexes.
testQuerySettingsParameterized({
    find: {
        querySettingsA: {indexHints: {ns, allowedIndexes: [{a: 1, b: 1}, {$natural: 1}]}},
        querySettingsB: {indexHints: {ns, allowedIndexes: [{b: 1, a: 1}]}},
    },
    distinct: {
        querySettingsA: {indexHints: {ns, allowedIndexes: [{a: 1, b: 1}, {$natural: 1}]}},
        querySettingsB: {indexHints: {ns, allowedIndexes: [{b: 1, a: 1}]}},
    },
    aggregate: {
        querySettingsA: {indexHints: {ns, allowedIndexes: [{groupID: 1, matchKey: 1}, {$natural: 1}]}},
        querySettingsB: {indexHints: {ns, allowedIndexes: [{matchKey: 1, groupID: 1}]}},
    },
});

// Test changing reject. With no other settings present, there's only one valid value for
// reject - true. Tests attempting to change this value to false will fail, as they are
// required to issue a removeQuerySettings instead.
// However, for the sake of coverage, test what can be tested when reject is the only
// setting present.
testQuerySettingsParameterized({
    find: {querySettingsA: {reject: true}, querySettingsB: {reject: true}},
    distinct: {querySettingsA: {reject: true}, querySettingsB: {reject: true}},
    aggregate: {querySettingsA: {reject: true}, querySettingsB: {reject: true}},
});

// Test setting and changing comment.
const commonSettingsA = {
    indexHints: {ns, allowedIndexes: [{a: 1, b: 1}, {$natural: 1}]},
    reject: true,
};
const commonSettingsB = {
    indexHints: {ns, allowedIndexes: [{a: 1, b: 1}]},
    reject: true,
};
testQuerySettingsParameterized({
    find: {
        querySettingsA: {...commonSettingsA, comment: "Reject this query, because..."},
        querySettingsB: {...commonSettingsB, comment: "Please set reject to false, because..."},
    },
    distinct: {
        querySettingsA: {...commonSettingsA, comment: "Reject this query, because..."},
        querySettingsB: {...commonSettingsB, comment: "Please set reject to false, because..."},
    },
    aggregate: {
        querySettingsA: {...commonSettingsA, comment: "Reject this query, because..."},
        querySettingsB: {...commonSettingsB, comment: "Please set reject to false, because..."},
    },
});

// Test changing reject with an unrelated setting present. Additionally test that 'setQuerySettings'
// with 'reject' is idempotent.
{
    const unrelatedSettings = {indexHints: {ns, allowedIndexes: ["a_1", {$natural: 1}]}};
    const query = qsutils.makeFindQueryInstance({filter: {a: 15}});

    for (const reject of [true, false]) {
        qsutils.assertQueryShapeConfiguration([]);

        // Repeat the following 'setQuerySettings' command twice to test the retryability. The first
        // call inserts new query settings for 'query'. The second call updates the query
        // settings with the same value.
        for (const op in [`insert with reject: ${reject}`, `update with reject: ${reject}`]) {
            assert.commandWorked(
                db.adminCommand({setQuerySettings: query, settings: {...unrelatedSettings, reject}}),
                `the 'setQuerySettings' ${op} operation failed`,
            );
            qsutils.assertQueryShapeConfiguration([
                qsutils.makeQueryShapeConfiguration({...unrelatedSettings, ...(reject && {reject})}, query),
            ]);
        }

        // Repeat the following 'setQuerySettings' command twice to test the retryability. The first
        // command updates the 'reject' setting with the opposite value. The second call updates the
        // query settings with the same value.
        for (const op in [`update with reject: ${!reject}`, `update with reject: ${!reject}`]) {
            assert.commandWorked(
                db.adminCommand({setQuerySettings: query, settings: {...unrelatedSettings, reject: !reject}}),
                `the 'setQuerySettings' ${op} operation failed`,
            );
            qsutils.assertQueryShapeConfiguration([
                qsutils.makeQueryShapeConfiguration({...unrelatedSettings, ...(!reject && {reject: !reject})}, query),
            ]);
        }

        db.adminCommand({removeQuerySettings: query});
        qsutils.assertQueryShapeConfiguration([]);
    }
}

// Test that making QuerySettings empty via setQuerySettings fails.
{
    // Test that setting reject=false as the _only_ setting fails as the newly constructed
    // QuerySettings would be empty.
    const query = qsutils.makeFindQueryInstance({filter: {a: 15}});
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {reject: false}}), 7746604);

    // Set reject=true, which should be permitted as it is non-default behaviour.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: {reject: true}}));

    // Setting reject=false would make the existing QuerySettings empty; verify that this fails.
    qsutils.assertQueryShapeConfiguration([qsutils.makeQueryShapeConfiguration({reject: true}, query)]);
    assert.commandFailedWithCode(db.adminCommand({setQuerySettings: query, settings: {reject: false}}), 7746604);

    // Confirm that the settings can indeed be removed (also cleans up after above test).
    assert.commandWorked(db.adminCommand({removeQuerySettings: query}));
    // Check that the given setting has indeed been removed.
    qsutils.assertQueryShapeConfiguration([]);
}

// Test that after setting via hash alone, on update the representative query gets set.
{
    const multitenancyParam = db.adminCommand({getParameter: 1, multitenancySupport: 1});
    const isMultitenancyEnabled = multitenancyParam.ok && multitenancyParam["multitenancySupport"];
    if (isMultitenancyEnabled) {
        quit();
    }

    const query = qsutils.makeFindQueryInstance({filter: {b: 1}});
    const queryShapeHash = "7F312F79FC0C37F532CBB024677C8D15641A290DA4F122B05BC5AE077CB314A7";
    const initialSettings = {queryFramework: "classic"};
    const finalSettings = {...initialSettings, reject: true};

    // Set the query settings via hash. Representative query will be missing.
    assert.commandWorked(db.adminCommand({setQuerySettings: queryShapeHash, settings: initialSettings}));
    qsutils.assertQueryShapeConfiguration([{settings: initialSettings}], false /* shouldRunExplain */);

    // Update the query settings via query. Representative query will be populated.
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: {reject: true}}));
    qsutils.assertQueryShapeConfiguration([qsutils.makeQueryShapeConfiguration(finalSettings, query)]);

    // Remove the query settings.
    assert.commandWorked(db.adminCommand({removeQuerySettings: queryShapeHash}));
    qsutils.assertQueryShapeConfiguration([]);
}

// Test that $querySettings works correctly with batchSize 1.
{
    const queryA = qsutils.makeFindQueryInstance({filter: {a: 15}});
    const queryB = qsutils.makeFindQueryInstance({filter: {b: 15}});
    const queryC = qsutils.makeFindQueryInstance({filter: {c: 15}});

    assert.commandWorked(db.adminCommand({setQuerySettings: queryA, settings: {reject: true}}));
    assert.commandWorked(db.adminCommand({setQuerySettings: queryB, settings: {reject: true}}));
    assert.commandWorked(db.adminCommand({setQuerySettings: queryC, settings: {reject: true}}));

    // Due to possible race conditions, we wrap the assertion into soon block.
    assert.soonNoExcept(() => {
        assert.sameMembers(
            [queryA, queryB, queryC],
            db
                .getSiblingDB("admin")
                .aggregate([{$querySettings: {}}, {$replaceRoot: {newRoot: "$representativeQuery"}}], {
                    cursor: {batchSize: 1},
                })
                .toArray(),
        );
        return true;
    });
}
