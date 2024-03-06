/*
 * Tests that the listDatabases command on the replica set endpoint returns the 'local' database.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagEmbeddedRouter,
 * ]
 */

function containsDbName(listDatabasesRes, dbName) {
    for (const database of listDatabasesRes.databases) {
        if (database.name == dbName) {
            return true;
        }
    }
    return false;
}

const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 1, setParameter: {featureFlagReplicaSetEndpoint: true}},
    configShard: true,
    embeddedRouter: true
});

const shard0Primary = st.rs0.getPrimary();
const router = new Mongo(shard0Primary.routerHost);

const dbName = "testDb";
const collName = "testColl";

assert.commandWorked(shard0Primary.getDB(dbName).getCollection(collName).insert({x: 1}));

// The listDatabase command on a router should still not return the 'local' database.
const routerRes = router.getDBs();
assert(containsDbName(routerRes, "config"), routerRes);
assert(containsDbName(routerRes, "admin"), routerRes);
assert(!containsDbName(routerRes, "local"), routerRes);
assert(containsDbName(routerRes, dbName), routerRes);

// The listDatabase command on the replica set endpoint should return the 'local' database.
const shard0PrimaryRes = shard0Primary.getDBs();
assert(containsDbName(shard0PrimaryRes, "config"), shard0PrimaryRes);
assert(containsDbName(shard0PrimaryRes, "admin"), shard0PrimaryRes);
assert(containsDbName(shard0PrimaryRes, "local"), shard0PrimaryRes);
assert(containsDbName(shard0PrimaryRes, dbName), shard0PrimaryRes);

st.stop();
