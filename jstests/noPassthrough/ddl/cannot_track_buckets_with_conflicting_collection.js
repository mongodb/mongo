/**
 * Tests that it's not possible to shard or move a malformed timeseries collection where both
 * 'coll' and 'system.buckets.coll' exist as collections (see SERVER-90862).
 *
 * @tags: [featureFlagCreateViewlessTimeseriesCollections_incompatible,
 *         featureFlagMarkTimeseriesEventsInOplog_incompatible]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, config: 1});
const db = st.s.getDB("test");
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard1.shardName}));

const coll = db.getCollection("coll");
const bucketsColl = db.getCollection("system.buckets.coll");

// Create both 'coll' and 'system.buckets.coll' as regular collections
assert.commandWorked(db.createCollection(bucketsColl.getName(), {timeseries: {timeField: "t"}}));
assert.commandWorked(db.createCollection(coll.getName()));

// Validate that we can't track them in the global catalog
assert.commandFailedWithCode(
    db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
    ErrorCodes.IllegalOperation,
);
const otherShardName = st.getOther(st.getPrimaryShard(db.getName())).shardName;
assert.commandFailedWithCode(
    db.adminCommand({moveCollection: coll.getFullName(), toShard: otherShardName}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.adminCommand({moveCollection: bucketsColl.getFullName(), toShard: otherShardName}),
    ErrorCodes.IllegalOperation,
);

// Drop to avoid a MalformedTimeseriesBucketsCollection checkMetadataConsistency failure on shutdown
db.dropDatabase();

st.stop();
