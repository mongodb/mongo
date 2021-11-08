// A dummy replicator that copies collections from one cluster to another,
// with the ability to filter specific collections to clone.

(function() {
'use strict';

load("jstests/libs/cluster_to_cluster_util.js");
load('jstests/libs/discover_topology.js');

jsTestLog(`Source connection: ${TestData.sourceConnectionString}, destination connection: ${
    TestData.destinationConnectionString}`);

// Copy the collection from one cluster to another.
function copyCollection(c0Conn, c1Conn, c0Topology, c1Topology, dbName, collInfo) {
    const collName = collInfo.name;
    jsTestLog(`Copying collection: ${dbName}.${collName}, ${tojson(collInfo)}`);

    // Create collection or view with the same option as in the source cluster.
    assert.commandWorked(c1Conn.getDB(dbName).createCollection(
        collName, Object.extend(collInfo.options, {writeConcern: {w: "majority"}})));

    // Skip all following operations if this is not of type collection (i.e. a view).
    if (collInfo.type !== "collection") {
        return;
    }

    // Create indexes on the destination collection except for _id index.
    const c0Coll = c0Conn.getDB(dbName).getCollection(collName);
    const c1Coll = c1Conn.getDB(dbName).getCollection(collName);
    for (const index of c0Coll.getIndexes()) {
        if (Object.keys(index.key).length !== 1 || !index.key._id || index.key._id === "hashed") {
            let options = Object.assign({}, index);
            delete options.key;
            assert.commandWorked(c1Coll.createIndex(index.key, options));
        }
    }

    // Retrieve shard key information from the source cluster and shard the
    // collection on the destination cluster.
    if (c0Topology.type === Topology.kShardedCluster &&
        c1Topology.type === Topology.kShardedCluster) {
        const shardKeyInfo = ClusterToClusterUtil.getShardKeyInfo(c0Conn, dbName, collName);
        // Skip if the collection is not sharded.
        if (shardKeyInfo) {
            assert.commandWorked(c1Conn.adminCommand({enableSharding: dbName}));
            assert.commandWorked(c1Conn.adminCommand({
                shardCollection: `${dbName}.${collName}`,
                key: shardKeyInfo.key,
                unique: shardKeyInfo.unique
            }));
        }
    }

    // Read and copy all collection data.
    const findRes = c0Coll.find({}).sort({_id: 1}).toArray();
    assert.commandWorked(c1Coll.insert(findRes));
}

// Create connections to both clusters, the connection string can represent a replica set
// primary in case of a replicaSet fixture or a mongos in case of a sharded cluster.
const c0Conn = new Mongo(TestData.sourceConnectionString);
const c1Conn = new Mongo(TestData.destinationConnectionString);
const c0Topology = DiscoverTopology.findConnectedNodes(c0Conn);
const c1Topology = DiscoverTopology.findConnectedNodes(c1Conn);

// Get the filtered collections and do copy.
const collInfoMap = ClusterToClusterUtil.getCollectionsToCopy(
    c0Conn, TestData.includeNamespaces, TestData.excludeNamespaces);
for (const [dbName, collInfos] of Object.entries(collInfoMap)) {
    jsTestLog(`Copying database: ${dbName}`);
    for (const collInfo of collInfos) {
        copyCollection(c0Conn, c1Conn, c0Topology, c1Topology, dbName, collInfo);
    }
}
})();
