/**
 * Test moveCollection command on tiemseries collections.
 *
 * @tags: [
 *   requires_timeseries,
 *   # we need 2 shards to perform moveCollection
 *   requires_2_or_more_shards,
 *   assumes_no_implicit_collection_creation_on_get_collection,
 * ]
 */

import {
    getRandomShardName,
} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

const coll = db[jsTestName()];
const timeField = 'time';
const metaField = 'component';

coll.drop();
assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

const docs = [
    {
        time: ISODate('2025-03-08T16:40:39.239Z'),
        component: "cpu",
        temp: 45,
    },
    {
        time: ISODate('2025-03-08T16:41:20.000Z'),
        component: "disk",
        temp: 30,
    },
    {
        time: ISODate('2025-03-08T16:42:30.131Z'),
        component: "gpu",
        temp: 80,
    },
    {
        time: ISODate('2025-03-08T16:43:40.283Z'),
        component: "cpu",
        temp: 50,
    }
];

assert.commandWorked(coll.insertMany(docs));

assert.sameMembers(docs, coll.find({}, {_id: 0}).toArray());

const primaryShardId = coll.getDB().getDatabasePrimaryShardId();
const otherShardId = getRandomShardName(db, [primaryShardId]);

// Move the collection to another shard
assert.commandWorked(db.adminCommand({moveCollection: coll.getFullName(), toShard: otherShardId}));
assert.sameMembers(docs, coll.find({}, {_id: 0}).toArray());

// Try to shard the collection
const shardCollCmd = {
    shardCollection: coll.getFullName(),
    key: {[metaField]: 1}
};
assert.commandWorked(db.adminCommand(shardCollCmd));
assert.sameMembers(docs, coll.find({}, {_id: 0}).toArray());
