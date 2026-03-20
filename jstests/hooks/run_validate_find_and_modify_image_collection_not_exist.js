/**
 * Runs after each test to assert that config.image_collection does not exist.
 * If it does exist, prints all documents in the collection and asserts.
 * Works against both replica set and sharded cluster.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a mongod?");

function checkConfigImageCollectionNotExist(conn, rsName) {
    const configDb = conn.getDB("config");
    const collNames = configDb.getCollectionNames();
    if (!collNames.includes("image_collection")) {
        return;
    }
    jsTest.log("config.image_collection exists on " + rsName + " after test:");
    const docs = configDb.image_collection.find();
    let count = 0;
    docs.forEach((doc) => {
        jsTest.log(tojsononeline(doc));
        count++;
    });
    jsTest.log("Found " + count + " document(s) in config.image_collection on " + rsName + ".");
    assert(false, "config.image_collection exists on " + rsName);
}

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

if (topology.type === Topology.kReplicaSet) {
    checkConfigImageCollectionNotExist(conn, "replica set " + conn.host);
} else if (topology.type === Topology.kShardedCluster) {
    for (const shardName of Object.keys(topology.shards)) {
        const shard = topology.shards[shardName];
        const shardHost = shard.type === Topology.kStandalone ? shard.mongod : shard.primary;
        const shardConn = newMongoWithRetry(shardHost);
        try {
            checkConfigImageCollectionNotExist(shardConn, "shard " + shardName + " (" + shardHost + ")");
        } finally {
            shardConn.close();
        }
    }
    const configPrimary = topology.configsvr.primary;
    const configConn = newMongoWithRetry(configPrimary);
    try {
        checkConfigImageCollectionNotExist(configConn, "config server (" + configPrimary + ")");
    } finally {
        configConn.close();
    }
} else {
    throw new Error("Unrecognized topology: " + tojson(topology));
}
