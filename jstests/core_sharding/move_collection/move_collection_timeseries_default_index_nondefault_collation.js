/**
 * Test moveCollection on a timeseries collection whose default {m:1, t: 1} index has
 * been dropped and then re-created with a non-default collation.
 *
 * @tags: [
 *   requires_timeseries,
 *   # we need 2 shards to perform moveCollection
 *   requires_2_or_more_shards,
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   # TODO SERVER-107141 re-enable this test in stepdown suites
 *   does_not_support_stepdowns,
 * ]
 */
import {getRandomShardName} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const coll = db[jsTestName()];

coll.drop();

// Create a timeseries collection and validate we have the default {m: 1, t: 1} index
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
{
    const index = coll.getIndexByKey({m: 1, t: 1});
    assert(index && !index.collation, tojson(index));
}

// Drop the default index and re-create it using a non-default collation
coll.dropIndexes();
assert.commandWorked(
    coll.createIndex(
        {m: 1, t: 1},
        {
            collation: {locale: "en_US", strength: 2},
        },
    ),
);

// Move the collection to another shard and check that the index still has the non-default collation
const primaryShardId = coll.getDB().getDatabasePrimaryShardId();
const otherShardId = getRandomShardName(db, [primaryShardId]);

assert.commandWorked(db.adminCommand({moveCollection: coll.getFullName(), toShard: otherShardId}));
{
    const index = coll.getIndexByKey({m: 1, t: 1});
    assert(index && index.collation && index.collation.locale == "en_US", tojson(index));
}
