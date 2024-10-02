// Tests that query settings have higher priority than index filters. Once query settings are set,
// index filters for the given query are ignored. When query settings are removed, index filters are
// applied again.
// @tags: [
//   requires_fcv_80,
// ]

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";
import {
    getQueryStatsFindCmd,
    getQueryStatsShapeHashes,
    resetQueryStatsStore,
    runOnReplsetAndShardedCluster
} from "jstests/libs/query_stats_utils.js";

runOnReplsetAndShardedCluster((conn, test) => {
    const db = conn.getDB("test");
    const coll = assertDropAndRecreateCollection(db, jsTestName());

    const qsutils = new QuerySettingsUtils(db, coll.getName());

    assert.commandWorked(coll.insertMany([
        {a: 1, b: 5},
        {a: 2, b: 4},
        {a: 3, b: 3},
        {a: 4, b: 2},
        {a: 5, b: 1},
    ]));

    const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 5}});
    const query = qsutils.withoutDollarDB(querySettingsQuery);
    const initialSettings = {queryFramework: "classic"};
    const finalSettings = {...initialSettings, reject: true};

    // Run the find command to generate a query stats store entry and get its query shape hash.
    assert.commandWorked(db.runCommand(query));
    const entries = getQueryStatsFindCmd(conn, {collName: coll.getName()});
    const queryShapeHashes = getQueryStatsShapeHashes(entries);
    assert.eq(queryShapeHashes.length, 1);

    // Set query settings via hash. Representative query is missing so we can't run explain yet.
    assert.commandWorked(
        db.adminCommand({setQuerySettings: queryShapeHashes[0], settings: initialSettings}));
    qsutils.assertQueryShapeConfiguration([{settings: initialSettings}],
                                          false /* shouldRunExplain */);

    // Update the query settings via query. Representative query will be populated and we can run
    // explain to make sure that the settings applied with the hash map to and apply to the correct
    // query shape.
    assert.commandWorked(
        db.adminCommand({setQuerySettings: querySettingsQuery, settings: {reject: true}}));
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(finalSettings, querySettingsQuery)]);

    // Cleanup.
    qsutils.removeAllQuerySettings();
});