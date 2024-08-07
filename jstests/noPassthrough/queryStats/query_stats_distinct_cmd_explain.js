/**
 * This test confirms that query stats is not collected for explains on distinct.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */
import {getQueryStatsDistinctCmd, withQueryStatsEnabled} from "jstests/libs/query_stats_utils.js";

const collName = jsTestName();
const viewName = "testView";

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();
    coll.insert({v: 1});
    coll.insert({v: 2});
    coll.insert({v: 3});
    coll.insert({v: 3});

    coll.explain("executionStats").distinct("v");
    let emptyDistinctQueryStats = getQueryStatsDistinctCmd(testDB);
    assert.eq(0, emptyDistinctQueryStats.length);

    assert.commandWorked(testDB.createView(viewName, collName, []));
    const view = testDB[viewName];

    view.explain("executionStats").distinct("v");
    let emptyViewDistinctQueryStats = getQueryStatsDistinctCmd(testDB);
    assert.eq(0, emptyViewDistinctQueryStats.length);
});
