/**
 * Tests that direct shard connections are correctly allowed and disallowed using authentication.
 *
 * @tags: [featureFlagFailOnDirectShardOperations, requires_fcv_73]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    name: jsTestName(),
    keyFile: "jstests/libs/key1",
    shards: 2,
    // Disallow chaining so that when testing replSetAbortPrimaryCatchUp we have the guarantee that
    // the secondary is syncing from the primary.
    rs0: {nodes: 3, settings: {chainingAllowed: false}},
});

const shardConn = st.rs0.getPrimary();
const shardAdminDB = shardConn.getDB("admin");

jsTest.log("Setup users for test");
shardAdminDB.createUser({user: "admin", pwd: 'x', roles: ["root"]});
assert(shardAdminDB.auth("admin", 'x'), "Authentication failed");
assert.commandWorked(shardAdminDB.runCommand(
    {setParameter: 1, logComponentVerbosity: {sharding: {verbosity: 2}, assert: {verbosity: 1}}}));
// The replSetStateChange action type is needed for this test
shardAdminDB.createUser({user: "user", pwd: "y", roles: ["clusterManager"]});
shardAdminDB.logout();

function awaitReplication(rst) {
    let nodes = rst.nodes;
    authutil.asCluster(nodes, "jstests/libs/key1", function() {
        rst.awaitReplication();
    });
}

function setupConn(conn, user, pwd) {
    let newConn = new Mongo(conn.host);
    assert(newConn.getDB("admin").auth(user, pwd), "Authentication failed");
    return newConn;
}

function runAsAdminUser(conn, cmd, database) {
    let newConn = setupConn(conn, "admin", "x");
    let res = assert.commandWorked(newConn.getDB(database).runCommand(cmd));
    newConn.close();
    return res;
}

jsTest.log("Await replication to ensure the user is created on all nodes");
awaitReplication(st.rs0);

jsTest.log("Testing replSetStepDown and replSetStepUp");
{
    let secondaryConn = setupConn(st.rs0.getSecondary(), "user", "y");
    assert.commandWorked(secondaryConn.adminCommand({replSetStepUp: 1}));
    authutil.asCluster(st.rs0.nodes, "jstests/libs/key1", function() {
        st.rs0.awaitNodesAgreeOnPrimary();
        st.rs0.awaitSecondaryNodes();
    });
    assert.soonNoExcept(() => {
        // {replSetStepDown: 0} defaults to 60 seconds rather than 0, so specify 1 here and unfreeze
        // the node later in the test if it will be stepped up to primary.
        return secondaryConn.adminCommand({replSetStepDown: 1, force: true}).ok;
    }, `failed attempt to step down node ${secondaryConn.host}`);
    authutil.asCluster(st.rs0.nodes, "jstests/libs/key1", function() {
        st.rs0.awaitNodesAgreeOnPrimary();
        st.rs0.awaitSecondaryNodes();
    });
    secondaryConn.close();
}

jsTest.log("Testing replSetFreeze");
{
    let secondaryConn = setupConn(st.rs0.getSecondary(), "user", "y");
    assert.commandWorked(secondaryConn.adminCommand({replSetFreeze: 1}));
    assert.commandWorked(secondaryConn.adminCommand({replSetFreeze: 0}));
    secondaryConn.close();
}

jsTest.log("Testing replSetMaintenance");
{
    let secondaryConn = setupConn(st.rs0.getSecondary(), "user", "y");
    assert.commandWorked(secondaryConn.adminCommand({replSetMaintenance: 1}));
    assert.commandWorked(secondaryConn.adminCommand({replSetMaintenance: 0}));
    secondaryConn.close();
}

jsTest.log("Testing replSetSyncFrom");
{
    let primary = st.rs0.getPrimary();
    let secondary1 = st.rs0.getSecondaries()[0];
    let secondary2Conn = setupConn(st.rs0.getSecondaries()[1], "user", "y");

    assert.commandWorked(secondary2Conn.adminCommand({replSetSyncFrom: secondary1.name}));
    assert.commandWorked(secondary2Conn.adminCommand({replSetSyncFrom: primary.name}));
    secondary2Conn.close();
}

jsTest.log("Testing replSetAbortPrimaryCatchUp");
{
    let primary = st.rs0.getPrimary();
    let secondary1 = st.rs0.getSecondaries()[0];
    let secondary2 = st.rs0.getSecondaries()[1];

    let primaryConn = setupConn(primary, "user", "y");
    let secondary1Conn = setupConn(secondary1, "user", "y");
    let secondary2Conn = setupConn(secondary2, "user", "y");
    // Ensure the secondary we plan to step up isn't frozen from a prior step down
    assert.commandWorked(secondary1Conn.adminCommand({replSetFreeze: 0}));

    // Reconfig to make the catchup timeout infinite.
    let newConfig = assert.commandWorked(primaryConn.adminCommand({replSetGetConfig: 1})).config;
    newConfig.version++;
    newConfig.settings.catchUpTimeoutMillis = -1;
    assert.commandWorked(primaryConn.adminCommand({replSetReconfig: newConfig}));
    // Put new secondary into primary catch up
    const stopReplProducerFailPoint2 = configureFailPoint(secondary2Conn, 'stopReplProducer');
    stopReplProducerFailPoint2.wait();
    runAsAdminUser(primary, {insert: "catch_up", documents: [{_id: 0}]}, "test");
    assert.soon(() => {
        let count = runAsAdminUser(secondary1,
                                   {
                                       count: "catch_up",
                                       readConcern: {level: "local"},
                                       $readPreference: {mode: "secondary"}
                                   },
                                   "test")
                        .n;
        return count == 1;
    });
    const stopReplProducerFailPoint1 = configureFailPoint(secondary1Conn, 'stopReplProducer');
    stopReplProducerFailPoint1.wait();
    runAsAdminUser(primary, {insert: "catch_up", documents: [{_id: 1}]}, "test");
    // Step up the first secondary, it will enter catch up indefinitely
    assert.commandWorked(secondary1Conn.adminCommand({replSetStepUp: 1}));

    // Now we can abort the catch up
    assert.commandWorked(secondary1Conn.adminCommand({replSetAbortPrimaryCatchUp: 1}));

    stopReplProducerFailPoint1.off();
    stopReplProducerFailPoint2.off();

    primaryConn.close();
    secondary1Conn.close();
    secondary2Conn.close();

    authutil.asCluster(st.rs0.nodes, "jstests/libs/key1", function() {
        st.rs0.awaitNodesAgreeOnPrimary();
        st.rs0.awaitSecondaryNodes();
    });
}

let primaryConn = setupConn(st.rs0.getPrimary(), "admin", "x");
let primaryAdminDB = primaryConn.getDB("admin");
primaryAdminDB.dropUser("user");
primaryAdminDB.dropUser("directOperationsUser");
primaryConn.close();

st.stop();
