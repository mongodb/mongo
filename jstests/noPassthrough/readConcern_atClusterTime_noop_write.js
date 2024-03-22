// Test that 'atClusterTime' triggers a noop write to advance the lastApplied optime if
// necessary.  This covers the case where a read is done at a cluster time that is only present
// as an actual opTime on another shard.
// @tags: [
//   requires_sharding,
//   uses_atclustertime,
//   uses_transactions,
// ]
import {getLastOpTime} from "jstests/replsets/rslib.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
if (!assert.commandWorked(conn.getDB("test").serverStatus())
         .storageEngine.supportsSnapshotReadConcern) {
    MongoRunner.stopMongod(conn);
    quit();
}
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2},
    other: {
        mongosOptions:
            {setParameter: {"failpoint.disableShardingUptimeReporting": "{mode: 'alwaysOn'}"}}
    }
});

// Create database "test0" on shard 0.
const testDB0 = st.s.getDB("test0");
assert.commandWorked(
    testDB0.adminCommand({enableSharding: testDB0.getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(testDB0.createCollection("coll0"));

// Create a database "test1" on shard 1.
const testDB1 = st.s.getDB("test1");
assert.commandWorked(
    testDB1.adminCommand({enableSharding: testDB1.getName(), primaryShard: st.shard1.shardName}));
assert.commandWorked(testDB1.createCollection("coll1"));

// we wait for the refresh spawned by shardCollection to complete. This is to prevent the clock of
// any shard from being ticked by an ongoing refresh while executing the test.
for (let conn of [st.rs0.getPrimary(), st.rs1.getPrimary()]) {
    let curOps = [];
    assert.soon(() => {
        curOps = conn.getDB("admin")
                     .aggregate([
                         {$currentOp: {allUsers: true}},
                         {$match: {"command._flushRoutingTableCacheUpdates": {$exists: true}}}
                     ])
                     .toArray();
        return curOps.length == 0;
    }, "Timed out waiting for create refreshes to finish, found: " + tojson(curOps));
}

let testNoopWrite = (fromDbName, fromColl, toRS, toDbName, toColl) => {
    const fromDBFromMongos = st.s.getDB(fromDbName);
    const toDBFromMongos = st.s.getDB(toDbName);

    const oplog = toRS.getPrimary().getCollection("local.oplog.rs");
    let findRes = oplog.findOne({o: {$eq: {"noop write for afterClusterTime read concern": 1}}});
    assert(!findRes);

    // Perform a write on the fromDB and get its op time.
    let res = assert.commandWorked(
        fromDBFromMongos.runCommand({insert: fromColl, documents: [{_id: 0}]}));
    assert(res.hasOwnProperty("operationTime"), tojson(res));
    let clusterTime = res.operationTime;

    let toRSOpTime = getLastOpTime(toRS.getPrimary()).ts;
    // In case 'from' shard has a clock already ahead of the 'to' shard, the entire test can be
    // skipped as the no-op write won't be executed.
    if (timestampCmp(toRSOpTime, clusterTime) >= 0) {
        return;
    }
    // Propagate 'clusterTime' to toRS. This ensures that its next write will be at time >=
    // 'clusterTime'.
    toDBFromMongos.getCollection(toColl).find().itcount();

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

    toRSOpTime = getLastOpTime(toRS.getPrimary()).ts;

    assert.gte(toRSOpTime, clusterTime);

    findRes = oplog.findOne({o: {$eq: {"noop write for afterClusterTime read concern": 1}}});
    assert(findRes);
};

//
// Test noop write. Read from the destination shard.
//

// The test requires the "to" shard to have its clock ahead the "from" shard, otherwise it early
// exits. Run the test both sides to make sure to execute the test at least once.
testNoopWrite("test0", "coll0", st.rs1, "test1", "coll1");
testNoopWrite("test1", "coll1", st.rs0, "test0", "coll0");

st.stop();
