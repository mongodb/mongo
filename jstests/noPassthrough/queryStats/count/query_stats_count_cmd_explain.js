/**
 * This tests confirms that query stats are not collected for explain for count.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */

import {getQueryStats, withQueryStatsEnabled} from "jstests/libs/query_stats_utils.js";

withQueryStatsEnabled(jsTestName(), (coll) => {
    const testDB = coll.getDB();
    const collName = jsTestName();
    assert.commandWorked(testDB.runCommand({explain: {count: collName}}));
    const stats = getQueryStats(testDB, {collName: collName});
    assert.eq(0, stats.length, stats);
});
