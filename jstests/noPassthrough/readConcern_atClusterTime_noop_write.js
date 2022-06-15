// Test that 'atClusterTime' triggers a noop write to advance the lastApplied optime if
// necessary.  This covers the case where a read is done at a cluster time that is only present
// as an actual opTime on another shard.
// @tags: [
//   requires_sharding,
//   uses_atclustertime,
//   uses_transactions,
// ]
(function() {
"use strict";
load("jstests/replsets/rslib.js");
load("jstests/libs/fail_point_util.js");

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
if (!assert.commandWorked(conn.getDB("test").serverStatus())
         .storageEngine.supportsSnapshotReadConcern) {
    MongoRunner.stopMongod(conn);
    return;
}
MongoRunner.stopMongod(conn);

// On the config server the lastApplied optime can go past the atClusterTime timestamp due to pings
// made on collection config.mongos or config.lockping by the distributed lock pinger thread and
// sharding uptime reporter thread. Hence, it will not write the no-op oplog entry on the config
// server as part of waiting for read concern.
// For more deterministic testing of no-op writes to the oplog, disable pinger threads from reaching
// out to the config server.
const failpointParams = {
    setParameter: {"failpoint.disableReplSetDistLockManager": "{mode: 'alwaysOn'}"}
};

// The ShardingUptimeReporter only exists on mongos.
const shardingUptimeFailpointName = jsTestOptions().mongosBinVersion == 'last-lts'
    ? "failpoint.disableShardingUptimeReporterPeriodicThread"
    : "failpoint.disableShardingUptimeReporting";
const mongosFailpointParams = {
    setParameter: {[shardingUptimeFailpointName]: "{mode: 'alwaysOn'}"}
};

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2},
    other: {
        configOptions: failpointParams,
        rsOptions: failpointParams,
        mongosOptions: mongosFailpointParams,
    }
});

// Create database "test0" on shard 0.
const testDB0 = st.s.getDB("test0");
assert.commandWorked(testDB0.adminCommand({enableSharding: testDB0.getName()}));
st.ensurePrimaryShard(testDB0.getName(), st.shard0.shardName);
assert.commandWorked(testDB0.createCollection("coll0"));

// Create a database "test1" on shard 1.
const testDB1 = st.s.getDB("test1");
assert.commandWorked(testDB1.adminCommand({enableSharding: testDB1.getName()}));
st.ensurePrimaryShard(testDB1.getName(), st.shard1.shardName);
assert.commandWorked(testDB1.createCollection("coll1"));

const PropagationPreferenceOptions = Object.freeze({kShard: 0, kConfig: 1});

let testNoopWrite = (fromDbName, fromColl, toRS, toDbName, toColl, propagationPreference) => {
    const fromDBFromMongos = st.s.getDB(fromDbName);
    const toDBFromMongos = st.s.getDB(toDbName);
    const configFromMongos = st.s.getDB("config");

    const oplog = toRS.getPrimary().getCollection("local.oplog.rs");
    let findRes = oplog.findOne({o: {$eq: {"noop write for afterClusterTime read concern": 1}}});
    assert(!findRes);

    // Perform a write on the fromDB and get its op time.
    let res = assert.commandWorked(
        fromDBFromMongos.runCommand({insert: fromColl, documents: [{_id: 0}]}));
    assert(res.hasOwnProperty("operationTime"), tojson(res));
    let clusterTime = res.operationTime;

    // Propagate 'clusterTime' to toRS or the config server. This ensures that its next
    // write will be at time >= 'clusterTime'. We cannot use toDBFromMongos to propagate
    // 'clusterTime' to the config server, because mongos only routes to the config server
    // for the 'config' and 'admin' databases.
    if (propagationPreference == PropagationPreferenceOptions.kConfig) {
        configFromMongos.coll1.find().itcount();
    } else {
        toDBFromMongos.toColl.find().itcount();
    }

    // Attempt a snapshot read at 'clusterTime' on toRS. Test that it performs a noop write
    // to advance its lastApplied optime past 'clusterTime'. The snapshot read itself may
    // fail if the noop write advances the node's majority commit point past 'clusterTime'
    // and it releases that snapshot.
    const toRSSession =
        toRS.getPrimary().getDB(toDBFromMongos).getMongo().startSession({causalConsistency: false});

    toRSSession.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
    res = toRSSession.getDatabase(toDBFromMongos).runCommand({find: toColl});
    if (res.ok === 0) {
        assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld);
        assert.commandFailedWithCode(toRSSession.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    } else {
        assert.commandWorked(toRSSession.commitTransaction_forTesting());
    }

    const toRSOpTime = getLastOpTime(toRS.getPrimary()).ts;

    assert.gte(toRSOpTime, clusterTime);

    findRes = oplog.findOne({o: {$eq: {"noop write for afterClusterTime read concern": 1}}});
    assert(findRes);
};

//
// Test noop write. Read from the destination shard.
//

testNoopWrite("test0", "coll0", st.rs1, "test1", "coll1", PropagationPreferenceOptions.kShard);

//
// Test noop write. Read from the config server's primary.
//

testNoopWrite(
    "test0", "coll2", st.configRS, "test1", "coll3", PropagationPreferenceOptions.kConfig);

st.stop();
}());
