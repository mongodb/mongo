// Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
// agg stage.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

// Creating the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

/**
 * Tests query settings setQuerySettings and removeQuerySettings commands as well as $querySettings
 * agg stage.
 *
 *  params.queryA - query of some shape
 *  params.queryShapeA - debugQueryShape of params.queryA
 *  params.queryB - query of shape different from A
 *  params.queryBPrime - different query of the same shape as params.queryB
 *  params.querySettingsA - query setting for all queries of the shape as params.queryA
 *  params.querySettingsB - query setting for all queries of the shape as params.queryB
 */
let testQuerySettingsUsing =
    (params) => {
        // Ensure that query settings cluster parameter is empty.
        { qsutils.assertQueryShapeConfiguration([]); }

        // Ensure that 'querySettings' cluster parameter contains QueryShapeConfiguration after
        // invoking setQuerySettings command.
        {
            qsutils.assertExplainQuerySettings(params.queryA, undefined);
            assert.commandWorked(db.adminCommand(
                {setQuerySettings: params.queryA, settings: params.querySettingsA}));
            qsutils.assertQueryShapeConfiguration(
                [qsutils.makeQueryShapeConfiguration(params.querySettingsA, params.queryA)]);
        }

        // Ensure that 'querySettings' cluster parameter contains both QueryShapeConfigurations
        // after invoking setQuerySettings command.
        {
            qsutils.assertExplainQuerySettings(params.queryB, undefined);
            assert.commandWorked(db.adminCommand(
                {setQuerySettings: params.queryB, settings: params.querySettingsB}));
            qsutils.assertQueryShapeConfiguration([
                qsutils.makeQueryShapeConfiguration(params.querySettingsA, params.queryA),
                qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryB)
            ]);
        }

        // Ensure that 'querySettings' cluster parameter gets updated on subsequent call of
        // setQuerySettings by passing a QueryShapeHash.
        {
            const queryShapeHashA = qsutils.getQueryHashFromQuerySettings(params.queryShapeA);
            assert.neq(queryShapeHashA, undefined);
            assert.commandWorked(db.adminCommand(
                {setQuerySettings: queryShapeHashA, settings: params.querySettingsB}));
            qsutils.assertQueryShapeConfiguration([
                qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryA),
                qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryB)
            ]);
        }

        // Ensure that 'querySettings' cluster parameter gets updated on subsequent call of
        // setQuerySettings by passing a different QueryInstance with the same QueryShape.
        {
            assert.commandWorked(db.adminCommand(
                {setQuerySettings: params.queryBPrime, settings: params.querySettingsA}));
            qsutils.assertQueryShapeConfiguration([
                qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryA),
                qsutils.makeQueryShapeConfiguration(params.querySettingsA, params.queryB)
            ]);
        }

        // Ensure that removeQuerySettings command removes one query settings from the
        // 'settingsArray' of the 'querySettings' cluster parameter by providing a query instance.
        {
            assert.commandWorked(db.adminCommand({removeQuerySettings: params.queryBPrime}));
            qsutils.assertQueryShapeConfiguration(
                [qsutils.makeQueryShapeConfiguration(params.querySettingsB, params.queryA)]);
            qsutils.assertExplainQuerySettings(params.queryB, undefined);
        }

        // Ensure that query settings cluster parameter is empty by issuing a removeQuerySettings
        // command providing a query shape hash.
        {
            qsutils.removeAllQuerySettings();
            qsutils.assertExplainQuerySettings(params.queryA, undefined);
        }
    }

// Testing find query settings.
testQuerySettingsUsing({
    queryA: qsutils.makeFindQueryInstance({filter: {a: 15}}),
    queryShapeA: {command: "find", filter: {a: {$eq: "?number"}}},
    queryB: qsutils.makeFindQueryInstance({filter: {b: "string"}}),
    queryBPrime: qsutils.makeFindQueryInstance({filter: {b: "another string"}}),
    querySettingsA: {indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}},
    querySettingsB: {indexHints: {allowedIndexes: ["b_1"]}},
});

// Same for distinct query settings.
testQuerySettingsUsing({
    queryA: qsutils.makeDistinctQueryInstance({key: "k", query: {a: 1}}),
    queryShapeA: {command: "distinct", key: "k", query: {a: {$eq: "?number"}}},
    queryB: qsutils.makeDistinctQueryInstance({key: "k", query: {b: "string"}}),
    queryBPrime: qsutils.makeDistinctQueryInstance({key: "k", query: {b: "another string"}}),
    querySettingsA: {indexHints: {allowedIndexes: ["a_1", {$natural: 1}]}},
    querySettingsB: {indexHints: {allowedIndexes: ["b_1"]}},
});

let buildPipeline = (matchValue) => [{$match: {matchKey: matchValue}},
                                     {
                                         $group: {
                                             _id: "groupID",
                                             values: {$addToSet: "$value"},
                                         },
                                     },
];

let buildPipelineShape = matchValue => {
    return {
        command: "aggregate", pipeline: [
            {$match: {matchKey: matchValue}},
            {$group: {_id: "?string", values: {$addToSet: "$value"}}}
        ],
    }
};

// Same for aggregate query settings.
testQuerySettingsUsing({
    queryA: qsutils.makeAggregateQueryInstance(buildPipeline(15)),
    queryShapeA: buildPipelineShape({$eq: "?number"}),
    queryB: qsutils.makeAggregateQueryInstance(buildPipeline("string")),
    queryBPrime: qsutils.makeAggregateQueryInstance(buildPipeline("another string")),
    querySettingsA: {indexHints: {allowedIndexes: ["groupID_1", {$natural: 1}]}},
    querySettingsB: {indexHints: {allowedIndexes: ["matchKey_1"]}},
});
