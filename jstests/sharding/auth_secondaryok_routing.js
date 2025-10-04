/**
 * This tests whether secondaryOk reads are properly routed through mongos in
 * an authenticated environment. This test also includes restarting the
 * entire set, then querying afterwards.
 *
 * This test involves a full restart of the replica set, so cannot be run with ephemeral storage
 * engines. When all nodes in a replica set are using an ephemeral storage engine, the set cannot
 * recover from a full restart. Once restarted, the nodes will have no knowledge of the replica set
 * config and will be unable to elect a primary.
 * @tags: [
 *   requires_persistence,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

// Replica set nodes started with --shardsvr do not enable key generation until they are added
// to a sharded cluster and reject commands with gossiped clusterTime from users without the
// advanceClusterTime privilege. This causes ShardingTest setup to fail because the shell
// briefly authenticates as __system and receives clusterTime metadata then will fail trying to
// gossip that time later in setup.
//

/**
 * Checks if a query to the given collection will be routed to the secondary. Returns true if
 * query was routed to a secondary node.
 */
function doesRouteToSec(coll, query) {
    let explain = coll.find(query).explain();
    assert.eq("SINGLE_SHARD", explain.queryPlanner.winningPlan.stage);
    let serverInfo = explain.queryPlanner.winningPlan.shards[0].serverInfo;
    let conn = new Mongo(serverInfo.host + ":" + serverInfo.port.toString());
    let cmdRes = conn.getDB("admin").runCommand({hello: 1});

    jsTest.log("hello: " + tojson(cmdRes));

    return cmdRes.secondary;
}

let rsOpts = {oplogSize: 50};
let st = new ShardingTest({
    shards: 1,
    rs: rsOpts,
    other: {keyFile: "jstests/libs/key1"},
    // By default, our test infrastructure sets the election timeout to a very high value
    // (24 hours). For this test, we need a shorter election timeout because it relies on
    // nodes running an election when they do not detect an active primary. Therefore, we
    // are setting the electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

let mongos = st.s;
let replTest = st.rs0;
let testDB = mongos.getDB("AAAAA");
let coll = testDB.user;
let nodeCount = replTest.nodes.length;

/* Add an admin user to the replica member to simulate connecting from
 * remote location. This is because mongod allows unautheticated
 * connections to access the server from localhost connections if there
 * is no admin user.
 */
let adminDB = mongos.getDB("admin");
adminDB.createUser({user: "user", pwd: "password", roles: jsTest.adminUserRoles});
adminDB.auth("user", "password");
if (!TestData.configShard) {
    // In config shard mode, creating this user above also created it on the first shard.
    var priAdminDB = replTest.getPrimary().getDB("admin");
    replTest.getPrimary().waitForClusterTime(60);
    priAdminDB.createUser({user: "user", pwd: "password", roles: jsTest.adminUserRoles}, {w: 3, wtimeout: 30000});
}

coll.drop();
coll.setSecondaryOk();

/* Secondaries should be up here, but they can still be in RECOVERY
 * state, which will make the ReplicaSetMonitor mark them as
 * ok = false and not eligible for secondaryOk queries.
 */
awaitRSClientHosts(mongos, replTest.getSecondaries(), {ok: true, secondary: true});

let bulk = coll.initializeUnorderedBulkOp();
for (let x = 0; x < 20; x++) {
    bulk.insert({v: x, k: 10});
}
assert.commandWorked(bulk.execute({w: nodeCount}));

/* Although mongos never caches query results, try to do a different query
 * everytime just to be sure.
 */
let vToFind = 0;

jsTest.log("First query to SEC");
assert(doesRouteToSec(coll, {v: vToFind++}));

let SIG_TERM = 15;
replTest.stopSet(SIG_TERM, true, {auth: {user: "user", pwd: "password"}});

for (let n = 0; n < nodeCount; n++) {
    replTest.restart(n, rsOpts);
}

replTest.awaitSecondaryNodes();

coll.setSecondaryOk();

/* replSetMonitor does not refresh the nodes information when getting secondaries.
 * A node that is previously labeled as secondary can now be a primary, so we
 * wait for the replSetMonitorWatcher thread to refresh the nodes information.
 */
awaitRSClientHosts(mongos, replTest.getSecondaries(), {ok: true, secondary: true});
//
// We also need to wait for the primary, it's possible that the mongos may think a node is a
// secondary but it actually changed to a primary before we send our final query.
//
awaitRSClientHosts(mongos, replTest.getPrimary(), {ok: true, ismaster: true});

// Recheck if we can still query secondaries after refreshing connections.
jsTest.log("Final query to SEC");
assert(doesRouteToSec(coll, {v: vToFind++}));

// Cleanup auth so Windows will be able to shutdown gracefully
priAdminDB = replTest.getPrimary().getDB("admin");
priAdminDB.auth("user", "password");
priAdminDB.dropUser("user");

st.stop();
