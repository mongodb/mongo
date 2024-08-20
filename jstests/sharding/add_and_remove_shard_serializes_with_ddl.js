/**
 * Tests that addShard and removeShard serialize with DDL operations.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   featureFlagStopDDLCoordinatorsDuringTopologyChanges,
 *   requires_fcv_80,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {waitForCommand} from "jstests/libs/wait_for_command.js";

let st = new ShardingTest({shards: 2, enableBalancer: true});

const dbName = "test";

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

let db = st.s.getDB(dbName);
let coll1 = db["coll1"];

// Test removeShard serializes with DDL.
{
    jsTest.log("Testing that removeShard serializes with DDL operations");

    coll1.insert({});

    // Start a DDL operation and block it.
    let fpBlockDropCollectionParticipant = configureFailPoint(st.rs0.getPrimary(), "failCommand", {
        failCommands: ["_shardsvrDropCollectionParticipant"],
        errorCode: ErrorCodes.HostUnreachable,
        namespace: coll1.getFullName(),
        failInternalCommands: true,
    });

    let awaitDropCollection = startParallelShell(() => {
        assert(db.getSiblingDB('test')['coll1'].drop());
    }, st.s.port);
    fpBlockDropCollectionParticipant.wait();

    // Start remove shard. It should block behind the running DDL.
    let shardToRemove = st.shard1.shardName;
    let awaitRemoveShard = startParallelShell(
        funWithArgs(async function(shardToRemove) {
            const {removeShard} = await import("jstests/sharding/libs/remove_shard_util.js");
            removeShard(db, shardToRemove);
        }, shardToRemove), st.s.port);

    // Wait for removeShard to reach the point where it waits for DDLs to drain.
    waitForCommand("_shardsvrJoinDDLCoordinators",
                   op => (op["command"] && op["command"]["_shardsvrJoinDDLCoordinators"] == 1),
                   st.rs0.getPrimary().getDB("admin"));

    // Check new DDL operations can still start and complete.
    assert.commandWorked(db.coll2.insert({}));
    assert(db.coll2.drop());

    // Check that the shard to be removed still exists and is draining.
    assert.eq(true, st.s.getDB("config")["shards"].findOne({_id: shardToRemove}).draining);

    // Unblock the blocked DDL operation
    fpBlockDropCollectionParticipant.off();
    awaitDropCollection();

    // Remove shard should now complete.
    awaitRemoveShard();
    assert.eq(undefined, st.s.getDB("config")["shards"].findOne({_id: shardToRemove}));
}

// Test addShard serializes with DDL.
{
    jsTest.log("Testing that addShard serializes with DDL operations");

    coll1.insert({});

    // Start a DDL operation and block it.
    let fpBlockDropCollectionParticipant = configureFailPoint(st.rs0.getPrimary(), "failCommand", {
        failCommands: ["_shardsvrDropCollectionParticipant"],
        errorCode: ErrorCodes.HostUnreachable,
        namespace: coll1.getFullName(),
        failInternalCommands: true,
    });

    let awaitDropCollection = startParallelShell(() => {
        assert(db.getSiblingDB('test')['coll1'].drop());
    }, st.s.port);
    fpBlockDropCollectionParticipant.wait();

    // Start add shard. It should block behind the running DDL.
    let shardToAdd = st.shard1.shardName;
    let shardToAddUrl = st.rs1.getURL();
    let awaitAddShard =
        startParallelShell(funWithArgs(async function(shardToAdd, shardToAddUrl) {
                               db.adminCommand({addShard: shardToAddUrl, name: shardToAdd});
                           }, shardToAdd, shardToAddUrl), st.s.port);

    // Wait for removeShard to reach the point where it waits for DDLs to drain.
    waitForCommand("_shardsvrJoinDDLCoordinators",
                   op => (op["command"] && op["command"]["_shardsvrJoinDDLCoordinators"] == 1),
                   st.rs0.getPrimary().getDB("admin"));

    // Check new DDL operations can still start and complete.
    assert.commandWorked(db.coll2.insert({}));
    assert(db.coll2.drop());

    // Check that the shard still has not been added
    assert.eq(0, st.s.getDB("config")["shards"].count({_id: shardToAdd}));

    // Unblock the blocked DDL operation
    fpBlockDropCollectionParticipant.off();
    awaitDropCollection();

    // Add shard should now complete.
    awaitAddShard();
    assert.eq(1, st.s.getDB("config")["shards"].count({_id: shardToAdd}));
}

// Test that if addShard/removeShard fails after having blocked new DDL coordinators, it will
// unblock them.
{
    jsTest.log(
        "Testing that the addOrRemoveShardInProgress is reset after a crash during add/removeShard");

    function getAddOrRemoveShardInProgressParamValue() {
        return assert
            .commandWorked(st.configRS.getPrimary().adminCommand(
                {getClusterParameter: "addOrRemoveShardInProgress"}))
            .clusterParameters[0]
            .inProgress;
    }

    function test(testCase) {
        let fp = configureFailPoint(st.configRS.getPrimary(), "hangRemoveShardAfterDrainingDDL");

        // Start remove shard.
        let shardToRemove = st.shard1.shardName;
        let awaitRemoveShard = startParallelShell(
            funWithArgs(async function(shardToRemove) {
                const {removeShard} = await import("jstests/sharding/libs/remove_shard_util.js");
                try {
                    removeShard(db, shardToRemove);
                } catch (e) {
                }
            }, shardToRemove), st.s.port);

        fp.wait();

        assert.eq(true, getAddOrRemoveShardInProgressParamValue());
        assert.eq(1,
                  st.configRS.getPrimary().getDB("admin")["system.version"].count(
                      {_id: "addOrRemoveShardInProgressRecovery"}));

        if (testCase === "killOp") {
            let configPrimary = st.configRS.getPrimary();
            let removeShardOpId = configPrimary.getDB("admin")
                                      .aggregate([
                                          {$currentOp: {allUsers: true}},
                                          {$match: {"command._configsvrRemoveShard": shardToRemove}}
                                      ])
                                      .toArray()[0]
                                      .opid;
            assert.commandWorked(configPrimary.getDB("admin").killOp(removeShardOpId));
        } else if (testCase === "stopServer") {
            // Restart the configsvr.
            st.stopAllConfigServers({} /* opts */, true /* forRestart */);
            st.restartAllConfigServers();
            // Take again references to db and coll1, since in embedded router suites they may be
            // invalid after restarting the configsvr.
            db = st.s.getDB(dbName);
            coll1 = db["coll1"];
        }

        awaitRemoveShard();

        // Soon the cluster parameter will be reset and new DDL operations will be able to start.
        assert.soon(() => {
            return getAddOrRemoveShardInProgressParamValue() === false &&
                st.configRS.getPrimary().getDB("admin")["system.version"].count(
                    {_id: "addOrRemoveShardInProgressRecovery"}) === 0;
        });

        assert.commandWorked(coll1.insert({}));
        assert(coll1.drop());
    }

    test("killOp");
    test("stopServer");
}

st.stop();
