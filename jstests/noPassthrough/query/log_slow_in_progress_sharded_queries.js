/**
 * Confirms that long-running operations are logged once during their progress in a sharded cluster.
 * @tags: [requires_sharding]
 */

import {findSlowInProgressQueryLogLine} from "jstests/libs/log.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertSlowInProgressQueryLogged(db, comment) {
    const logLine = findSlowInProgressQueryLogLine(db, comment);
    assert.neq(null, logLine, "Did not find slow in-progress query log line for " + comment);
}

const kDocCount = 2048;

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
});

const db = st.s.getDB("log_slow_in_progress_queries");
const coll = db.test;

assert.commandWorked(db.dropDatabase());
assert.commandWorked(db.setProfilingLevel(0, {slowinprogms: 0}));

// Enable sharding on the database and shard the collection.
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

const docs = [];
for (let i = 0; i < kDocCount; ++i) {
    docs.push({a: i});
}

function setup_coll(coll) {
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndex({a: 1}));
}

setup_coll(coll);

assert.eq(kDocCount, coll.find({}).comment("Collection Scan").itcount());
assertSlowInProgressQueryLogged(db, "Collection Scan");

assert.eq(
    kDocCount,
    coll
        .find({a: {$gte: 0}})
        .comment("Index Scan")
        .itcount(),
);
assertSlowInProgressQueryLogged(db, "Index Scan");

assert.eq(kDocCount, coll.aggregate([{$match: {a: {$gte: 0}}}], {comment: "Agg Index Scan"}).itcount());
assertSlowInProgressQueryLogged(db, "Agg Index Scan");

assert.eq(
    kDocCount,
    db.aggregate([{$documents: docs}, {$match: {a: {$gte: 0}}}], {comment: "Agg Documents"}).itcount(),
);
assertSlowInProgressQueryLogged(db, "Agg Documents");

assert.commandWorked(
    db.runCommand({
        update: "test",
        updates: [
            {q: {a: {$gte: 0, $lt: 512}}, u: {$inc: {u: 1}}, multi: true},
            {q: {a: {$gte: 512, $lt: 1024}}, u: {$inc: {u: 1}}, multi: true},
            {q: {a: {$gte: 1024, $lt: 1536}}, u: {$inc: {u: 1}}, multi: true},
            {q: {a: {$gte: 1536}}, u: {$inc: {u: 1}}, multi: true},
        ],
        comment: "Update Index Scan",
    }),
);
assertSlowInProgressQueryLogged(db, "Update Index Scan");

assert.commandWorked(
    db.runCommand({
        delete: "test",
        deletes: [{q: {a: {$gte: 0}}, limit: 0}],
        comment: "Delete Index Scan",
    }),
);
assertSlowInProgressQueryLogged(db, "Delete Index Scan");

// Clean up after tests.
st.stop();
