/* Tests default read/write concern value consistency before, during, and after promotion to
 * sharded.
 * @tags: [
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // for stop and restart replication on secondaries

const dbName = "test";
const collName = "foo";

// This function expects the implicit defaults to be:
//   defaultWriteConcern: {w: "majority", wtimeout: 0}
//   defaultReadConcern: {level: "local"}
function checkImplicitDefaults(rs, counter) {
    jsTestLog("Stop replication on secondaries");
    stopReplicationOnSecondaries(rs, false);

    jsTestLog("Do a write, it should time out due to missing majority");
    assert.commandFailedWithCode(
        rs.getPrimary().getDB(dbName).runCommand(
            {insert: collName, documents: [{x: counter}], maxTimeMS: 500}),
        ErrorCodes.MaxTimeMSExpired,
    );

    jsTestLog("Do a read, it should return the document anyways since the default is local");
    let docFound = rs.getPrimary().getDB(dbName).getCollection(collName).count({x: counter});
    assert.eq(1, docFound);

    counter++;

    jsTestLog("Restart replication and wait for steady");
    restartReplicationOnSecondaries(rs);
    rs.awaitReplication();

    return counter;
}

// This function expects the user defaults to be:
//   defaultWriteConcern: {w: "majority", wtimeout: 500}
//   defaultReadConcern: {level: "majority"}
// These values were chosen both for ease of testing and to ensure we aren't overlapping
// with the implicit or empty constructor defaults.
function checkUserSpecifiedDefaults(rs, counter) {
    jsTestLog("Stop replication on secondaries");
    stopReplicationOnSecondaries(rs, false);

    jsTestLog("Do a write, it should time out due to missing majority");
    assert.commandFailedWithCode(
        rs.getPrimary().getDB(dbName).runCommand({insert: collName, documents: [{x: counter}]}),
        ErrorCodes.WriteConcernFailed,
    );

    jsTestLog("Do a read, it should not return the document since the read concern is majority");
    let docFound = rs.getPrimary().getDB(dbName).getCollection(collName).count({x: counter});
    assert.eq(0, docFound);

    counter++;

    jsTestLog("Restart replication and wait for steady");
    restartReplicationOnSecondaries(rs);
    rs.awaitReplication();

    return counter;
}

function setupReplSet(shardsvr) {
    let verbosity = {replication: 2, command: 2};
    let rs = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            setParameter: {logComponentVerbosity: verbosity},
        },
    });
    if (shardsvr) {
        rs.startSet({"shardsvr": ""});
    } else {
        rs.startSet();
    }
    rs.initiate();

    assert.commandWorked(rs.getPrimary().getDB(dbName).createCollection(collName));

    return rs;
}

jsTestLog("Implicit defaults during promotion to sharded");
{
    let replSet = setupReplSet(false /* shardsvr */);
    let counter = 1;

    jsTestLog("Check implicit defaults as a normal replica set");
    counter = checkImplicitDefaults(replSet, counter);

    jsTestLog("Check implicit defaults when started with --shardsvr");
    replSet.stopSet(null, true, {});
    replSet.startSet({"shardsvr": ""}, true);
    counter = checkImplicitDefaults(replSet, counter);

    jsTestLog("Check implicit defaults after being added to the cluster");
    let cluster = new ShardingTest({shards: 0});
    assert.commandWorked(cluster.s.adminCommand({addShard: replSet.getURL()}));
    // Fetch the sharding metadata so that the write doesn't have to do a refresh.
    assert.commandWorked(
        replSet.getPrimary().adminCommand(
            {_flushRoutingTableCacheUpdates: dbName + "." + collName}),
    );
    counter = checkImplicitDefaults(replSet, counter);

    cluster.stop();
    replSet.stopSet();
}

jsTestLog("User specified defaults during promotion to sharded");
{
    let replSet = setupReplSet(false /* shardsvr */);
    let cluster = new ShardingTest({shards: 0});
    let counter = 1;

    jsTestLog("Set the defaults to something other than the implicit default");
    let conns = [replSet.getPrimary(), cluster.s];
    conns.forEach((conn) => {
        assert.commandWorked(
            conn.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: "majority", wtimeout: 500},
                defaultReadConcern: {level: "majority"},
                writeConcern: {w: "majority"},
            }),
        );
    });

    jsTestLog("Check user specified defaults as a normal replica set");
    counter = checkUserSpecifiedDefaults(replSet, counter);

    jsTestLog("Check user specified defaults when started with --shardsvr");
    replSet.stopSet(null, true, {});
    replSet.startSet({"shardsvr": ""}, true);
    replSet.awaitReplication();
    counter = checkUserSpecifiedDefaults(replSet, counter);

    jsTestLog("Check user specified defaults after being added to the cluster");
    assert.commandWorked(cluster.s.adminCommand({addShard: replSet.getURL()}));
    // Fetch the sharding metadata so that the write doesn't have to do a refresh.
    assert.commandWorked(
        replSet.getPrimary().adminCommand(
            {_flushRoutingTableCacheUpdates: dbName + "." + collName}),
    );
    counter = checkUserSpecifiedDefaults(replSet, counter);

    cluster.stop();
    replSet.stopSet();
}

jsTestLog("Changing the default on the cluster does not change the shard's default");
{
    let replSet = setupReplSet(true /* shardsvr */);
    let cluster = new ShardingTest({shards: 0});
    let counter = 1;

    jsTestLog("Add the shard to the cluster");
    assert.commandWorked(cluster.s.adminCommand({addShard: replSet.getURL()}));
    replSet.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: dbName + "." + collName});

    jsTestLog("Change the defaults on the config server");
    assert.commandWorked(
        cluster.s.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1, wtimeout: 0},
            defaultReadConcern: {level: "majority"},
            writeConcern: {w: "majority"},
        }),
    );

    jsTestLog("Check that the defaults are still the implicit ones via direct connection");
    counter = checkImplicitDefaults(replSet, counter);

    jsTestLog("Verify that the defaults cannot be modified on the shard");
    assert.commandFailedWithCode(
        replSet.getPrimary().adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1, wtimeout: 0},
            defaultReadConcern: {level: "majority"},
            writeConcern: {w: "majority"},
        }),
        51301,
    );

    cluster.stop();
    replSet.stopSet();
}
}());
