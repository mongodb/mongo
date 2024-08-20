// Test that 'atClusterTime' triggers a noop write to advance the lastApplied optime if
// necessary.  This covers the case where a read is done at a cluster time that is only present
// as an actual opTime on another shard.
// @tags: [
//   requires_sharding,
//   uses_atclustertime,
//   uses_transactions,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getLastOpTime} from "jstests/replsets/rslib.js";

function toNs(dbName, collName) {
    return dbName + "." + collName;
}
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

/*
 * Create one unsharded collection per shard
 * SHARD0 : "test0.coll0"
 * SHARD1 : "test1.coll1"
 */
var collectionMap = {};
collectionMap[st.shard0.shardName] = {
    rs: st.rs0,
    dbName: "test0",
    collName: "coll0"
};
collectionMap[st.shard1.shardName] = {
    rs: st.rs1,
    dbName: "test1",
    collName: "coll1"
};

// Create an unsharded collection per shard.
const createCollectionOnShard = shard => {
    const dbName = collectionMap[shard.shardName].dbName;
    const collName = collectionMap[shard.shardName].collName;
    const fullName = toNs(dbName, collName);

    assert.commandWorked(
        st.s.getDB(dbName).adminCommand({enableSharding: dbName, primaryShard: shard.shardName}));
    assert.commandWorked(st.s.getDB(dbName).createCollection(collName));

    // We refresh the metadata forcefully. This is to prevent the clock of any shard from being
    // ticked by an ongoing refresh while executing the test.
    assert.commandWorked(
        shard.rs.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: fullName}));
};

createCollectionOnShard(st.shard0);
createCollectionOnShard(st.shard1);

let testNoopWrite = (sourceShardName, destinationShardName) => {
    var fromDbName = collectionMap[sourceShardName].dbName;
    var fromCollName = collectionMap[sourceShardName].collName;

    var toDbName = collectionMap[destinationShardName].dbName;
    var toCollName = collectionMap[destinationShardName].collName;
    var toRS = collectionMap[destinationShardName].rs;

    jsTest.log(
        `Testing source shard ${sourceShardName}, destination shard ${destinationShardName}`);

    const oplog = toRS.getPrimary().getCollection("local.oplog.rs");
    let findRes = oplog.findOne({o: {$eq: {"noop write for afterClusterTime read concern": 1}}});
    assert(!findRes);

    // Perform a write on the fromDB and get its op time.
    let res = assert.commandWorked(
        st.s.getDB(fromDbName).runCommand({insert: fromCollName, documents: [{_id: 0}]}));
    assert(res.hasOwnProperty("operationTime"), tojson(res));
    let fromRSOpTime = res.operationTime;

    let toRSOpTime = getLastOpTime(toRS.getPrimary()).ts;
    // In case 'to' shard has a clock already ahead of the 'from' shard, the entire test can be
    // skipped as the no-op write won't be executed.
    if (timestampCmp(toRSOpTime, fromRSOpTime) >= 0) {
        jsTest.log(`Skipping check for source shard ${sourceShardName}, destination shard ${
            destinationShardName}. Destination shard's clock ${
            tojson(toRSOpTime)} >= Source shard's clock ${tojson(fromRSOpTime)}`);
        return;
    }

    // Propagate 'fromRSOpTime' to toRS. This ensures that its next write will be at time >=
    // 'clusterTime'.
    st.s.getDB(toDbName).getCollection(toCollName).find().itcount();

    // Attempt a snapshot read at 'fromRSOpTime' on toRS. Test that it performs a noop write
    // to advance its lastApplied optime past 'fromRSOpTime'. The snapshot read itself may
    // fail if the noop write advances the node's majority commit point past 'fromRSOpTime'
    // and it releases that snapshot.
    const toRSSession =
        toRS.getPrimary().getDB(toDbName).getMongo().startSession({causalConsistency: false});

    jsTest.log(`Running transaction with snapshot read concern ${tojson(fromRSOpTime)}`);

    // In case the shard does not advance its lastOpTime, the transaction would hang indefinitely.
    toRSSession.startTransaction({readConcern: {level: "snapshot", atClusterTime: fromRSOpTime}});
    res = toRSSession.getDatabase(toDbName).runCommand({find: toCollName, maxTimeMS: 60 * 1000});
    assert.neq(res.code,
        ErrorCodes.MaxTimeMSExpired,
        `Transaction with read concern snapshot at clusterTime ${tojson(fromRSOpTime)} did not complete on time. The shard might not have advanced its lastOpTime before executing.`);
    if (res.ok === 0) {
        assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld);
        assert.commandFailedWithCode(toRSSession.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
        return;
    }

    assert.commandWorked(toRSSession.commitTransaction_forTesting());
    // Check the lastOpTime advanced. This should happen either because of another operation, or
    // because of a no-op in the oplog performed by the transaction itself.
    toRSOpTime = getLastOpTime(toRS.getPrimary()).ts;
    assert.gte(toRSOpTime, fromRSOpTime);
};

//
// Test noop write. Read from the destination shard.
//

// The test requires the "to" shard to have its clock ahead the "from" shard, otherwise it early
// exits. Run the test both sides to make sure to execute the test at least once.
testNoopWrite(st.shard0.shardName, st.shard1.shardName);
testNoopWrite(st.shard1.shardName, st.shard0.shardName);

st.stop();
