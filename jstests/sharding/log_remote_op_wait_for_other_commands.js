/**
 * Tests that command log lines which execute remote operations include remoteOpWaitMillis: the
 * amount of time the merger spent waiting for results from shards.
 *
 * @tags: [
 * # The 'remoteOpWaitMillis' was added to explain output for commands other than $mergeCursor
 * # aggregation stage in 6.3.
 * requires_fcv_63
 * ]
 */
(function() {

load("jstests/libs/log.js");  // For findMatchingLogLine.

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
st.stopBalancer();

const dbName = st.s.defaultDB;
const coll = st.s.getDB(dbName).getCollection('profile_remote_op_wait');

coll.drop();
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Shards the test collection and splits it into two chunks: one that contains all {shard: 1}
// documents and one that contains all {shard: 2} documents.
st.shardColl(coll.getName(),
             {shard: 1} /* shard key */,
             {shard: 2} /* split at */,
             {shard: 2} /* move the chunk containing {shard: 2} to its own shard */,
             dbName,
             true);

assert.commandWorked(
    coll.insert(Array.from({length: 100}, (_, i) => ({_id: i, shard: (i % 2) + 1, x: i}))));

// Sets the slow query logging threshold (slowMS) to -1 to ensure every query gets logged.
st.s.getDB('admin').setProfilingLevel(0, -1);

function getRemoteOpWait(logLine) {
    const pattern = /remoteOpWaitMillis"?:([0-9]+)/;
    const match = logLine.match(pattern);
    assert(match, `pattern ${pattern} did not match line: ${logLine}`);
    const millis = parseInt(match[1]);
    assert.gte(millis, 0, match);
    return millis;
}

function getDuration(logLine) {
    const pattern = /durationMillis"?:([0-9]+)/;
    const match = logLine.match(pattern);
    assert(match, `pattern ${pattern} did not match line: ${logLine}`);
    const millis = parseInt(match[1]);
    assert.gte(millis, 0, match);
    return millis;
}

// An .find() includes remoteOpWaitMillis.
const findComment = 'example_find_should_have_remote_op_wait_too';
coll.find().sort({x: 1}).comment(findComment).next();
{
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    const lines =
        [...findMatchingLogLines(mongosLog.log, {msg: "Slow query", comment: findComment})];
    const line = lines.find(line => line.match(/command.{1,4}find/));
    assert(line, "Failed to find a 'find' log line matching the comment");
    assert(line.match(/remoteOpWait/), `Log line does not contain remoteOpWait: ${line}`);
    const remoteOpWait = getRemoteOpWait(line);
    const duration = getDuration(line);
    assert.lte(remoteOpWait, duration);
}

// Other commands which execute remote operations also include remoteOpWaitMillis, such as
// listCollections.
const listCollectionsComment = 'example_listCollections_should_have_remote_op_wait_too';
coll.runCommand({listCollections: 1, comment: listCollectionsComment});
{
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    const lines = [...findMatchingLogLines(mongosLog.log,
                                           {msg: "Slow query", comment: listCollectionsComment})];
    const line = lines.find(line => line.match(/command.{1,4}listCollections/));
    assert(line, "Failed to find a 'listCollections' log line matching the comment");
    assert(line.match(/remoteOpWait/), `Log line does not contain remoteOpWait: ${line}`);
    const remoteOpWait = getRemoteOpWait(line);
    const duration = getDuration(line);
    assert.lte(remoteOpWait, duration);
}

st.stop();
})();
