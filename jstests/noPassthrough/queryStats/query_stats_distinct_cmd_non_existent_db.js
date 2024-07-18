/**
 * Test that mongos is collecting query stats metrics for distinct queries on non-existent
 * databases.
 * @tags: [featureFlagQueryStatsCountDistinct]
 */

import {getLatestQueryStatsEntry, withQueryStatsEnabled} from "jstests/libs/query_stats_utils.js";

const collName = jsTestName();

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();

    const nonExistentDB = testDB.getSiblingDB("newDB");
    assert.eq([], nonExistentDB.anything.distinct("v"));
    const entry = getLatestQueryStatsEntry(testDB, {collName: "anything"});
    assert.neq(null, entry);
});
