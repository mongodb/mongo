/*
 * Test that fixing an invalid expireAfterSeconds value via collMod on step-up behaves normally,
 * even if there's a prepared transaction on the collection.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    makeCommitTransactionCmdObj,
    makePrepareTransactionCmdObj,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
let shard0Primary = st.rs0.getPrimary();

const kDbName = "testDb";
const kCollName = "testColl";
let shard0TestDB = st.rs0.getPrimary().getDB(kDbName);
let shard0TestColl = shard0TestDB.getCollection(kCollName);

const nonIntVal = NaN;
const intVal = 2147483647;

assert.commandWorked(shard0TestDB.createCollection(kCollName));
const fp1 = configureFailPoint(shard0Primary, 'skipTTLIndexValidationOnCreateIndex');
const fp2 = configureFailPoint(shard0Primary, 'skipTTLIndexExpireAfterSecondsValidation');
try {
    assert.commandWorked(
        shard0TestDB[kCollName].createIndex({t: 1}, {expireAfterSeconds: nonIntVal}));
} finally {
    fp2.off();
    fp1.off();
}

let catalog = shard0TestColl.aggregate([{$listCatalog: {}}]).toArray();
assert.eq(catalog[0].db, shard0TestDB.getName());
assert.eq(catalog[0].name, shard0TestColl.getName());
assert.eq(catalog[0].md.indexes.length, 2);
assert.eq(catalog[0].md.indexes[0].spec.name, "_id_");
assert.eq(catalog[0].md.indexes[1].spec.expireAfterSeconds, nonIntVal);

function stepDownShard0Primary() {
    jsTestLog("Beginning step down");
    const oldPrimary = st.rs0.getPrimary();
    const oldSecondary = st.rs0.getSecondary();
    assert.commandWorked(oldSecondary.adminCommand({replSetFreeze: 0}));
    assert.commandWorked(
        oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    const newPrimary = st.rs0.getPrimary();
    assert.neq(oldPrimary, newPrimary);
    shard0Primary = newPrimary;
    shard0TestDB = shard0Primary.getDB(kDbName);
    shard0TestColl = shard0TestDB.getCollection(kCollName);
    jsTestLog("Step down completed");
}

const docToInsert = {
    x: 1,
    t: ISODate("2001-01-01T00:00:00.000Z"),
};
const stmtId = 1;

const lsid = {
    id: UUID(),
    txnNumber: NumberLong(35),
    txnUUID: UUID()
};
const childTxnNumber = NumberLong(0);

const originalWriteCmdObj = {
    insert: kCollName,
    documents: [docToInsert],
    lsid: lsid,
    txnNumber: NumberLong(childTxnNumber),
    startTransaction: true,
    autocommit: false,
    stmtId: NumberInt(stmtId),
};
const prepareCmdObj = makePrepareTransactionCmdObj(lsid, childTxnNumber);
const commitCmdObj = makeCommitTransactionCmdObj(lsid, childTxnNumber);

assert.commandWorked(shard0TestDB.runCommand(originalWriteCmdObj));
const preparedTxnRes = assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
commitCmdObj.commitTimestamp = preparedTxnRes.prepareTimestamp;

stepDownShard0Primary();

assert.commandWorked(shard0TestDB.adminCommand(commitCmdObj));
assert.eq(shard0TestColl.count(docToInsert), 1);

assert.soon(
    () => {
        return 1 ===
            st.findOplog(shard0Primary,
                         {
                             op: 'c',
                             ns: shard0TestDB.getCollection('$cmd').getFullName(),
                             'o.collMod': shard0TestColl.getName(),
                             'o.index.name': 't_1',
                             'o.index.expireAfterSeconds': intVal
                         },
                         /*limit=*/ 1)
                .toArray()
                .length;
    },
    'TTL index with ' + nonIntVal +
        ' expireAfterSeconds was not fixed using collMod during step-up: ' +
        tojson(st.findOplog(shard0Primary, {op: {$ne: 'n'}}, /*limit=*/ 10).toArray()));

catalog = shard0TestColl.aggregate([{$listCatalog: {}}]).toArray();
assert.eq(catalog[0].db, shard0TestDB.getName());
assert.eq(catalog[0].name, shard0TestColl.getName());
assert.eq(catalog[0].md.indexes.length, 2);
assert.eq(catalog[0].md.indexes[0].spec.name, "_id_");
assert.eq(catalog[0].md.indexes[1].spec.expireAfterSeconds, intVal);

st.stop();
