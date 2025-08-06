/**
 * Tests that when "searchScore" and "vectorSearchScore" metadata fields are referenced in an
 * invalid way, a warning log line will occasionally be printed to the logs.
 *
 * The test in with_mongot/e2e/metadata/search_score_meta_validation_warning_log.js is responsible
 * for testing that the warning log lines are _not_ printed when the metadata is referenced
 * correctly.
 *
 * The testing must be separate since this test requires noPassthrough to manually check the
 * correct node's logs, but the other test requires a real mongot to run valid $search/$vectorSearch
 * queries. We do not have the infrastructure to run a noPassthrough test on a real mongot.
 *
 * @tags: [requires_fcv_82]
 */
import {iterateMatchingLogLines} from "jstests/libs/log.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const warningLogMessage =
    "The query attempts to retrieve metadata at a place in the pipeline where that metadata type is not available";
const searchScoreWarningFieldMatcher = {
    msg: warningLogMessage,
    attr: {metadataType: "$search score"}
};
const vectorSearchScoreWarningFieldMatcher = {
    msg: warningLogMessage,
    attr: {metadataType: "$vectorSearch score"}
};

const dbName = "test";
const collName = "search_score_meta_validation";

const data = [
    {a: 1, b: "foo", score: 10},
    {a: 2, b: "bar", score: 20},
    {a: 3, b: "baz", score: 30},
    {a: 4, b: "qux", score: 40},
    {a: 5, b: "quux", score: 50},
];

function checkLogs(db, warningLogFieldMatcher, numLogLines) {
    const globalLogs = db.adminCommand({getLog: 'global'});
    const matchingLogLines = [...iterateMatchingLogLines(globalLogs.log, warningLogFieldMatcher)];
    assert.eq(matchingLogLines.length, numLogLines, matchingLogLines);
}

/**
 * Tests that the metadata validation warning log line is printed when the metadata is used in an
 * invalid way.
 *
 * Commands are sent to db but the logs are checked in the provided logDb. For a sharded cluster,
 * commands will be sent to the mongos and the logs will be checked on the primary shard.
 */
function warningLogTest(db, warningLogFieldMatcher, metaType, logDb) {
    assert.commandWorked(logDb.adminCommand({clearLog: "global"}));

    // Assert that the warning message is not logged before the command is even run.
    checkLogs(logDb, warningLogFieldMatcher, 0);

    assert.commandWorked(db.runCommand({
        aggregate: collName,
        pipeline: [
            {$setWindowFields: {sortBy: {score: {$meta: metaType}}, output: {rank: {$rank: {}}}}},
        ],
        cursor: {}
    }));

    // Now that we have run the command, make sure the warning message is logged once.
    checkLogs(logDb, warningLogFieldMatcher, 1);

    // It will be printed once every 64 queries (every 128 ticks, with two ticks per query since
    // dependency tracking is called twice per query). Now run 64 more invalid queries (4 queries *
    // 32 iterations) to get another log line.
    for (let i = 0; i < 16; i++) {
        assert.commandWorked(db.runCommand(
            {aggregate: collName, pipeline: [{$sort: {score: {$meta: metaType}}}], cursor: {}}));
        assert.commandWorked(db.runCommand(
            {aggregate: collName, pipeline: [{$set: {score: {$meta: metaType}}}], cursor: {}}));

        assert.commandWorked(db.runCommand({
            find: collName,
            projection: {score: {$meta: metaType}},
        }));
        assert.commandWorked(db.runCommand({
            find: collName,
            sort: {score: {$meta: metaType}},
        }));
    }

    // Check that we only had one more log.
    checkLogs(logDb, warningLogFieldMatcher, 2);
}

jsTest.log.info('Test standalone');
{
    const mongod = MongoRunner.runMongod({});
    const db = mongod.getDB(dbName);
    const coll = db.getCollection(collName);
    coll.drop();
    assert.commandWorked(coll.insertMany(data));

    warningLogTest(db, searchScoreWarningFieldMatcher, "searchScore", db);
    checkLogs(db, vectorSearchScoreWarningFieldMatcher, 0);
    warningLogTest(db, vectorSearchScoreWarningFieldMatcher, "vectorSearchScore", db);
    checkLogs(db, searchScoreWarningFieldMatcher, 0);

    MongoRunner.stopMongod(mongod);
}

jsTest.log.info('Test sharded');
{
    const shardingTest = new ShardingTest({shards: 2, mongos: 1});
    const session = shardingTest.s.getDB(dbName).getMongo().startSession();
    const shardedDB = session.getDatabase(dbName);
    const primaryShardConn = shardingTest.shard0.getDB(dbName);

    assert.commandWorked(shardingTest.s0.adminCommand(
        {enableSharding: shardedDB.getName(), primaryShard: shardingTest.shard0.shardName}));

    const shardedColl = shardedDB.getCollection(collName);

    assert.commandWorked(shardedDB.createCollection(shardedColl.getName()));

    assert.commandWorked(
        shardingTest.s0.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

    assert.commandWorked(shardedColl.insert(data));

    warningLogTest(shardedDB, searchScoreWarningFieldMatcher, "searchScore", primaryShardConn);
    checkLogs(primaryShardConn, vectorSearchScoreWarningFieldMatcher, 0);
    warningLogTest(
        shardedDB, vectorSearchScoreWarningFieldMatcher, "vectorSearchScore", primaryShardConn);
    checkLogs(primaryShardConn, searchScoreWarningFieldMatcher, 0);

    shardingTest.stop();
}
