// Tests the behavior of looking up the post image for change streams on collections which are
// sharded with a hashed shard key.
// @tags: [
//   does_not_support_stepdowns,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    enableBalancer: false,
    rs: {
        nodes: 1,
        // Use a higher frequency for periodic noops to speed up the test.
        setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
    }
});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB['coll'];

assert.commandWorked(mongosDB.dropDatabase());

// Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
assert.commandWorked(
    mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.rs0.getURL()}));

// Shard the test collection on the field "shardKey".
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {shardKey: "hashed"}}));

// TODO SERVER-81884: update once 8.0 becomes last LTS.
if (!FeatureFlagUtil.isPresentAndEnabled(mongosDB,
                                         "OneChunkPerShardEmptyCollectionWithHashedShardKey")) {
    assert.commandWorked(mongosDB.adminCommand({
        mergeChunks: mongosColl.getFullName(),
        bounds: [{shardKey: MinKey}, {shardKey: NumberLong("0")}]
    }));
    assert.commandWorked(mongosDB.adminCommand({
        mergeChunks: mongosColl.getFullName(),
        bounds: [{shardKey: NumberLong("0")}, {shardKey: MaxKey}]
    }));
}

// Make sure the negative chunk is on shard 0.
assert.commandWorked(mongosDB.adminCommand({
    moveChunk: mongosColl.getFullName(),
    bounds: [{shardKey: MinKey}, {shardKey: NumberLong("0")}],
    to: st.rs0.getURL()
}));

// Make sure the positive chunk is on shard 1.
assert.commandWorked(mongosDB.adminCommand({
    moveChunk: mongosColl.getFullName(),
    bounds: [{shardKey: NumberLong("0")}, {shardKey: MaxKey}],
    to: st.rs1.getURL()
}));

assert.soon(() => {
    let lastInserted = -1;
    let lastUpdated = -1;
    let lastInsertSeen = -1;
    let lastUpdateSeen = -1;
    let resumeToken = "";
    let finished = false;

    while (!finished) {
        try {
            const changeStream = resumeToken
                ? mongosColl.aggregate(
                      [{$changeStream: {fullDocument: "updateLookup", resumeAfter: resumeToken}}])
                : mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}]);
            resumeToken = changeStream._resumeToken;

            // Write enough documents that we likely have some on each shard.
            const nDocs = 1000;
            for (let id = Math.min(lastInserted, lastUpdated) + 1; id < nDocs; ++id) {
                if (id > lastInserted) {
                    assert.commandWorked(mongosColl.insert({_id: id, shardKey: id}));
                    lastInserted = id;
                }
                if (id > lastUpdated) {
                    assert.commandWorked(
                        mongosColl.update({shardKey: id}, {$set: {updatedCount: 1}}));
                    lastUpdated = id;
                }
            }

            for (let id = Math.min(lastInsertSeen, lastUpdateSeen) + 1; id < nDocs; ++id) {
                if (id > lastInsertSeen) {
                    assert.soon(() => changeStream.hasNext());
                    let next = changeStream.next();
                    assert.eq(next.operationType, "insert");
                    assert.eq(next.documentKey, {shardKey: id, _id: id});
                    resumeToken = next._id;
                    lastInsertSeen = id;
                }

                if (id > lastUpdateSeen) {
                    assert.soon(() => changeStream.hasNext());
                    let next = changeStream.next();
                    assert.eq(next.operationType, "update");
                    assert.eq(next.documentKey, {shardKey: id, _id: id});
                    assert.docEq({_id: id, shardKey: id, updatedCount: 1}, next.fullDocument);
                    resumeToken = next._id;
                    lastUpdateSeen = id;
                }
            }
            finished = true;
        } catch (e) {
            // In a step down the changeStream may return QueryPlanKilled, which can be recovered
            if (!(ErrorCodes.isCursorInvalidatedError(e) || isRetryableError(e))) {
                throw (e);
            }
        }
        return true;
    }
});

st.stop();
