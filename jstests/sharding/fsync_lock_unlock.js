/**
 * Verifies the fsync with lock+unlock command on mongos.
 * @tags: [
 *   requires_fsync,
 *   uses_parallel_shell,
 * ]
 */
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

const dbName = "test";
const collName = "collTest";
const ns = dbName + "." + collName;
const st =
    new ShardingTest({shards: 2, mongos: 1, config: 1, configShard: true, enableBalancer: true});
const adminDB = st.s.getDB('admin');
const distributed_txn_insert_count = 10;

function waitUntilOpCountIs(opFilter, num, st) {
    assert.soon(() => {
        let ops = st.s.getDB('admin')
                      .aggregate([
                          {$currentOp: {}},
                          {$match: opFilter},
                      ])
                      .toArray();
        if (ops.length != num) {
            jsTest.log("Num operations: " + ops.length + ", expected: " + num);
            jsTest.log(ops);
            return false;
        }
        return true;
    });
}

async function runTransaction() {
    const {withTxnAndAutoRetryOnMongos} =
        await import("jstests/libs/auto_retry_transaction_in_sharding.js");

    // Start the transaction and insert a document.
    const sessionOptions = {causalConsistency: false};
    const session = db.getSiblingDB("test").getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase("test");
    const sessionColl = sessionDb["collTest"];

    session.endSession();

    withTxnAndAutoRetryOnMongos(session, () => {
        for (let i = 0; i < 10; i++) {
            assert.commandWorked(sessionColl.insert({x: i}));
        }
    }, {});

    jsTest.log("END txn in parallel shell");
}

let collectionCount = 1;
const performFsyncLockUnlockWithReadWriteOperations = function() {
    if (jsTestOptions().embeddedRouter) {
        // TODO (SERVER-84243): Dedicate a catalog cache and loader to the shard role. If we don't
        // explicitly create the test collection here, the insert below would trigger a
        // CatalogCache refresh on this embedded router. Embedded routers currently use the
        // ShardServerCatalogCacheLoader. So when a refresh occurs, it requires doing a noop
        // write which would then deadlock since the cluster is fsync locked.
        assert.commandWorked(st.s.getDB("test").createCollection("collTest"));
    }
    // lock then unlock
    assert.commandWorked(st.s.adminCommand({fsync: 1, lock: true}));

    // Make sure writes are blocked. Spawn a write operation in a separate shell and make sure it
    // is blocked. There is really no way to do that currently, so just check that the write didn't
    // go through.
    let codeToRun = () => {
        assert.commandWorked(db.getSiblingDB("test").getCollection("collTest").insert({x: 1}));
    };

    let writeOpHandle = startParallelShell(codeToRun, st.s.port);

    waitUntilOpCountIs({op: 'insert', ns: 'test.collTest', waitingForLock: true}, 1, st);

    // Make sure reads can still run even though there is a pending write and also that the write
    // didn't get through.
    assert.eq(collectionCount, coll.count());
    assert.commandWorked(st.s.adminCommand({fsyncUnlock: 1}));

    writeOpHandle();

    // ensure writers are allowed after the cluster is unlocked
    assert.commandWorked(coll.insert({x: 1}));
    collectionCount += 2;
    assert.eq(coll.count(), collectionCount);

    // Ensure that distributed transactions are blocked when the cluster is locked
    assert.commandWorked(st.s.adminCommand({fsync: 1, lock: true}));

    let txnOpHandle = startParallelShell(runTransaction, st.s.port);

    // Verify that txns are unsuccessful when cluster is locked.
    assert.eq(collectionCount, st.s.getCollection(coll.getFullName()).countDocuments({}));
    assert.commandWorked(st.s.adminCommand({fsyncUnlock: 1}));

    txnOpHandle();
    collectionCount += distributed_txn_insert_count;

    // Verify that txns are successful after cluster is unlocked.
    assert.eq(collectionCount, st.s.getCollection(coll.getFullName()).countDocuments({}));

    // Ensure that fsync (lock: false) still works by performing a write after invoking the command,
    // and checking the write is successful, showing the cluster does not need to be unlocked.
    assert.commandWorked(st.s.adminCommand({fsync: 1, lock: false}));
    assert.commandWorked(coll.insert({x: 2}));
    collectionCount += 1;
    assert.eq(coll.count(), collectionCount);
};

jsTest.log("Insert some data.");
const coll = st.s0.getDB(dbName)[collName];
assert.commandWorked(coll.insert({x: 1}));

// unlock before lock should fail
let ret = assert.commandFailed(st.s.adminCommand({fsyncUnlock: 1}));
let errmsg = "fsyncUnlock called when not locked";
assert.eq(ret.errmsg.includes(errmsg), true);

performFsyncLockUnlockWithReadWriteOperations();

// Make sure the lock and unlock commands still work as expected after transitioning to a dedicated
// config server.
moveDatabaseAndUnshardedColls(st.s.getDB(dbName), st.shard1.shardName);

ShardTransitionUtil.transitionToDedicatedConfigServer(st);
performFsyncLockUnlockWithReadWriteOperations();

st.stop();
