// Checks that the data on both clusters involved in a cluster to cluster replication is consistent.
'use strict';

(function() {
load("jstests/libs/cluster_to_cluster_util.js");
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
load("jstests/libs/namespace_utils.js");

const includeNamespaces = TestData.includeNamespaces;
const excludeNamespaces = TestData.excludeNamespaces;

const conn0 = new Mongo(TestData.sourceConnectionString);
const conn1 = new Mongo(TestData.destinationConnectionString);

jsTestLog(`Source connection: ${TestData.sourceConnectionString}, destination connection: ${
    TestData.destinationConnectionString}`);
function checkCollection(conn0, conn1, ns, shouldCheckShardKey) {
    const coll0 = conn0.getCollection(ns);
    const coll1 = conn1.getCollection(ns);

    jsTestLog(`Checking indexes for namespace: ${ns}`);
    const indexes0 = coll0.getIndexes().sort();
    const indexes1 = coll1.getIndexes().sort();
    assert.eq(indexes0,
              indexes1,
              `Indexes were not matching for: ${ns}. indexes0: ${tojson(indexes0)}, indexes1: ${
                  tojson(indexes1)}`);

    // Ensure the shard keys match, if necessary.
    if (shouldCheckShardKey) {
        jsTestLog(`Checking shard keys for namespace: ${ns}`);
        const keyInfo0 = conn0.getCollection('config.collections').findOne({_id: ns});
        const keyInfo1 = conn1.getCollection('config.collections').findOne({_id: ns});

        if (!keyInfo0 || !keyInfo1) {
            assert.eq(keyInfo0, keyInfo1, `Both shards keys should be 'null'.`);
        } else {
            assert.eq(keyInfo0.key, keyInfo1.key, `Shard keys did not match for the two clusters.`);
        }
    }

    jsTestLog(`Checking documents for namespace: ${ns}`);
    const cursor0 = coll0.find().sort({_id: 1});
    const cursor1 = coll1.find().sort({_id: 1});

    const diff = ((diff) => {
        return {
            docsWithDifferentContents: diff.docsWithDifferentContents.map(
                ({first, second}) => ({cluster0: first, cluster1: second})),
            docsExtra: diff.docsMissingOnFirst,
            docsMissing: diff.docsMissingOnSecond,
        };
    })(DataConsistencyChecker.getDiff(cursor0, cursor1));

    assert.eq(diff,
              {
                  docsWithDifferentContents: [],
                  docsExtra: [],
                  docsMissing: [],
              },
              `The two clusters have different contents for namespace ${ns}`);
}

jsTestLog("Ensuring that the collection metadata matches.");
const collsToCopyPerDB0 =
    ClusterToClusterUtil.getCollectionsToCopy(conn0, includeNamespaces, excludeNamespaces);
// We want to view all the collections on cluster1, to make sure that the replicator only cloned
// the collections in 'includeNamespaces' or correctly excluded the collections in
// 'excludeNamespaces'.
const collsCopiedPerDB1 = ClusterToClusterUtil.getCollectionsToCopy(conn1, [], []);

for (const [dbName, collInfos0] of Object.entries(collsToCopyPerDB0)) {
    collInfos0.forEach((item) => {
        delete item.info.uuid;
    });
    collInfos0.sort();

    assert(collsCopiedPerDB1.hasOwnProperty(dbName),
           `Cluster1 does not contain the database ${dbName}: ${tojson(collsCopiedPerDB1)}`);
    const collInfos1 = collsCopiedPerDB1[dbName];

    collInfos1.forEach((item) => {
        delete item.info.uuid;
    });
    collInfos1.sort();
}

assert.eq(
    collsToCopyPerDB0, collsCopiedPerDB1, `The two clusters do not have the same collections.`);

const topology0 = DiscoverTopology.findConnectedNodes(conn0);
const topology1 = DiscoverTopology.findConnectedNodes(conn1);
const shouldCheckShardKey =
    topology0.type == Topology.kShardedCluster && topology1.type == Topology.kShardedCluster;

// Examine each collection.
for (const [dbName, collInfos] of Object.entries(collsToCopyPerDB0)) {
    collInfos.forEach((item) => {
        // Skip validating the collection if this is not of collection type.
        if (item.type !== "collection")
            return;

        checkCollection(conn0, conn1, `${dbName}.${item.name}`, shouldCheckShardKey);
    });
}
})();
