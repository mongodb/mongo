/**
 * Test that query stats are collected for distinct queries on non-existent
 * databases.
 * @tags: [requires_fcv_81]
 */

import {
    getLatestQueryStatsEntry,
    withQueryStatsEnabled
} from "jstests/libs/query/query_stats_utils.js";

const collName = "anything";
const viewName = "testView";

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();

    const nonExistentDB = testDB.getSiblingDB("newDB");
    const nonExistentColl = nonExistentDB[collName];
    assert.eq([], nonExistentColl.distinct("v"));
    const entry = getLatestQueryStatsEntry(testDB, {collName: collName});
    assert.neq(null, entry);

    // Create a view on a nonexistent database and confirm query stats are collected.
    assert.commandWorked(nonExistentDB.createView(viewName, collName, []));
    const nonExistentView = nonExistentDB[viewName];

    assert.eq([], nonExistentView.distinct("v"));
    const viewEntry = getLatestQueryStatsEntry(testDB, {collName: viewName});
    assert.neq(null, viewEntry);
});
