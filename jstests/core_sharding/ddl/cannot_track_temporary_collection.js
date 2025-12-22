/**
 * Tests that temporary collections (i.e. with temp=true in the collection options)
 * can not be tracked in the global catalog. Those collections are inherently local,
 * and are dropped upon step up in a sharding-unaware way.
 *
 * @tags: [
 *   # The test tries to move a collection across shards.
 *   requires_2_or_more_shards,
 *   # Creates temporary collections, which are always unsharded.
 *   assumes_unsharded_collection,
 *   # Temporary collections are dropped on stepdowns.
 *   does_not_support_stepdowns,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getRandomShardName} from "jstests/libs/sharded_cluster_fixture_helpers.js";

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
const primaryShardId = db.getDatabasePrimaryShardId();
const otherShardId = getRandomShardName(db, [primaryShardId]);
const topology = DiscoverTopology.findConnectedNodes(db);
const primaryShardConn = new Mongo(topology.shards[primaryShardId].primary);

// Temporary collections are only created internally by operations such as convertToCapped,
// and can not be created by users. For testing purposes, create one via applyOps.
const coll = db.getCollection("tmpcoll");
assert(coll.drop());
assert.commandWorked(
    primaryShardConn.adminCommand({
        applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {create: coll.getName(), temp: true}}],
    }),
);

assert.commandFailedWithCode(
    db.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}),
    ErrorCodes.IllegalOperation,
);
assert.commandFailedWithCode(
    db.adminCommand({moveCollection: coll.getFullName(), toShard: otherShardId}),
    ErrorCodes.IllegalOperation,
);

// Since the collection isn't tracked, it can be dropped locally without causing a
// MissingLocalCollection inconsistency.
assert(primaryShardConn.getDB(db.getName()).getCollection(coll.getName()).drop());
const inconsistencies = db.checkMetadataConsistency().toArray();
assert.eq(0, inconsistencies.length, tojson(inconsistencies));
