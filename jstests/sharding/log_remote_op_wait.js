/**
 * Test that aggregation log lines include remoteOpWaitMillis: the amount of time the merger spent
 * waiting for results from shards.
 *
 * @tags: [
 * # $mergeCursors was added to explain output in 5.3.
 * requires_fcv_53
 * ]
 *
 */
(function() {

load("jstests/libs/log.js");  // For findMatchingLogLine.

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
st.stopBalancer();

const dbName = st.s.defaultDB;
const coll = st.s.getDB(dbName).getCollection('profile_remote_op_wait');

coll.drop();
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));

// Shard the test collection and split it into two chunks: one that contains all {shard: 1}
// documents and one that contains all {shard: 2} documents.
st.shardColl(coll.getName(),
             {shard: 1} /* shard key */,
             {shard: 2} /* split at */,
             {shard: 2} /* move the chunk containing {shard: 2} to its own shard */,
             dbName,
             true);

assert.commandWorked(
    coll.insert(Array.from({length: 100}, (_, i) => ({_id: i, shard: (i % 2) + 1, x: i}))));

// We want a pipeline that:
// 1. Requires a mergerPart. Otherwise, the entire query might get passed through to one shard, and
//    we wouldn't spend time waiting for other nodes.
// 2. Is streaming. Otherwise, the merger would have to consume its entire input before returning
//    the first batch, meaning subsequent getMores wouldn't do any waiting.
// A merge-sort stage should satisfy both of these requirements.
const pipeline = [{$sort: {x: 1}}];
const pipelineComment = 'example_pipeline_should_have_remote_op_wait';

{
    const explain = coll.explain().aggregate(pipeline);
    assert(explain.shards, explain);
    assert.eq(2, Object.keys(explain.shards).length, explain);
    assert.eq(explain.splitPipeline.shardsPart, [{$sort: {sortKey: {x: 1}}}], explain);
    // The mergerPart will only have a $mergeCursors stage that merge-sorts the results from each
    // shard.
    assert.eq(1, explain.splitPipeline.mergerPart.length, tojson(explain));
    assert(explain.splitPipeline.mergerPart[0].hasOwnProperty("$mergeCursors"), tojson(explain));
    assert.eq({x: 1}, explain.splitPipeline.mergerPart[0]["$mergeCursors"]["sort"]);
}

// Set the slow query logging threshold (slowMS) to -1 to ensure every query gets logged.
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

// Run the pipeline and check mongos for the log line.
const cursor = coll.aggregate(pipeline, {comment: pipelineComment, batchSize: 1});
{
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    const line = findMatchingLogLine(mongosLog.log, {msg: "Slow query", comment: pipelineComment});
    assert(line, 'Failed to find a log line matching the comment');
    const remoteOpWait = getRemoteOpWait(line);
    const duration = getDuration(line);
    assert.lte(remoteOpWait, duration);
}

// Run a getMore and check again for the log line: .next() empties the current 1-document batch, and
// .hasNext() fetches a new batch.
cursor.next();
cursor.hasNext();
{
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    const lines =
        [...findMatchingLogLines(mongosLog.log, {msg: "Slow query", comment: pipelineComment})];
    const line = lines.find(line => line.match(/command.{1,4}getMore/));
    assert(line, 'Failed to find a getMore log line matching the comment');
    const remoteOpWait = getRemoteOpWait(line);
    const duration = getDuration(line);
    assert.lte(remoteOpWait, duration);
}

// A changestream is a type of aggregation, so it reports remoteOpWait. The initial $changeStream
// 'aggregate' command never pauses execution while awaiting data, and so we expect the remoteOpWait
// time to be less than or equal to the total execution duration.
const watchComment = 'example_watch_should_have_remote_op_wait';
const csCursor = coll.watch([], {comment: watchComment});
{
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    const line = findMatchingLogLine(mongosLog.log, {msg: "Slow query", comment: watchComment});
    assert(line, "Failed to find a log line matching the comment");
    const remoteOpWait = getRemoteOpWait(line);
    const duration = getDuration(line);
    assert.lte(remoteOpWait, duration);
}

// A $changeStream getMore may pause execution while awaiting data if no results are currently
// available. In this case, it is possible for the total execution time to be less than the
// remoteOpWait time.
assert(!csCursor.hasNext());
{
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    const line = findMatchingLogLine(
        mongosLog.log, {msg: "Slow query", comment: watchComment, command: "getMore"});
    assert(line, "Failed to find a log line matching the comment");
    const remoteOpWait = getRemoteOpWait(line);
    const duration = getDuration(line);
    assert.lte(duration, 100);
    assert.gte(remoteOpWait, 900);
}

// A query that merges on a shard logs remoteOpWaitMillis on the shard.
const pipeline2 = [{$sort: {x: 1}}, {$group: {_id: "$y"}}];
const pipelineComment2 = 'example_pipeline2_should_have_remote_op_wait';
{
    const explain2 = coll.explain().aggregate(pipeline2, {allowDiskUse: true});
    assert.eq(explain2.mergeType, 'anyShard', explain2);
}
st.shard0.getDB('admin').setProfilingLevel(0, -1);
st.shard1.getDB('admin').setProfilingLevel(0, -1);
coll.aggregate(pipeline2, {allowDiskUse: true, comment: pipelineComment2}).next();
{
    const shard0Log = assert.commandWorked(st.shard0.adminCommand({getLog: "global"}));
    const shard1Log = assert.commandWorked(st.shard1.adminCommand({getLog: "global"}));
    const bothShardsLogLines = shard0Log.log.concat(shard1Log.log);
    const lines = [...findMatchingLogLines(bothShardsLogLines,
                                           {msg: "Slow query", comment: pipelineComment2})];
    // The line we want is whichever had a $mergeCursors stage.
    const line = lines.find(line => line.match(/mergeCursors/));
    assert(line, `Failed to find a log line mentioning 'mergeCursors': ${lines}`);
    const remoteOpWait = getRemoteOpWait(line);
    const duration = getDuration(line);
    assert.lte(remoteOpWait, duration);
}

st.stop();
})();
