/**
 *  Tests that query stats won't be collected for the 'analyze' command.
 */
import {
    getQueryStats,
} from "jstests/libs/query_stats_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsErrorsAreCommandFatal: true}
});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());
const coll = db.analyze_coll;
assert.commandWorked(coll.insert({x: 5}));

assert.commandWorked(db.runCommand({analyze: coll.getName(), key: "x", sampleRate: 0.5}));

// The analyze command ends up running a complex aggregation pipeline - make sure that we didn't
// collect query stats for it.
assert.eq(
    getQueryStats(db), [], "Did not expect to find any query stats entries for analyze command");

MongoRunner.stopMongod(conn);
