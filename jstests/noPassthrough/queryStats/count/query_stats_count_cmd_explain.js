/**
 * This tests confirms that query stats are not collected for explain for count.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */

import {getQueryStats} from "jstests/libs/query_stats_utils.js";

const options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};
const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB("test");
assert.commandWorked(testDB.runCommand({explain: {count: jsTestName()}}));
const stats = getQueryStats(conn);
assert.eq(0, stats.length, stats);
MongoRunner.stopMongod(conn);
