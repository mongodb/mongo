import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";

const collName = "coll";
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
assert.commandWorked(
    db.adminCommand({shardCollection: `${db.getName()}.${collName}`, key: {_id: 1}}),
);

// Hang CheckMetadataConsistencyInBackground before it reads any data, then drop the collection
// directly on the shard to create a MissingLocalCollection inconsistency for it to catch.
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
const shardConn = new Mongo(Object.values(topology.shards)[0].primary);
const fp = configureFailPoint(db.getMongo(), "hangCheckMetadataBeforeEstablishCursors");
fp.wait();
assert(shardConn.getDB(db.getName()).getCollection(collName).drop());
fp.off();
