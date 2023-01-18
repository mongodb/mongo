/**
@tags: [multiversion_incompatible]
*/

// Don't check for UUID index consistency,orphans and routine table across the cluster at the end,
// since the test shuts down a mongos
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckShardFilteringMetadata = true;

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallelTester.js');
load("jstests/libs/retryable_writes_util.js");
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
});
const dbName = "test";
const collName = "mycoll";
const fpData = {
    "cmdName": "insert",
    "ns": dbName + '.' + collName
};
const hangBeforeCheckInterruptFailPoint =
    configureFailPoint(st.s, "hangBeforeCheckingMongosShutdownInterrupt", fpData);

const insertThread = new Thread(function insertDoc(host, dbName, collName) {
    var lsid = UUID();
    const conn = new Mongo(host);
    const retrySession = conn.startSession({retryWrites: true});
    const retrySessionDB = retrySession.getDatabase(dbName);

    try {
        var res = assert.commandWorked(retrySessionDB.runCommand({
            insert: 'mycoll',
            documents: [{x: 0}, {x: 1}],
            ordered: true,
            lsid: {id: lsid},
            txnNumber: NumberLong(1)
        }));

    } catch (e) {
        assert.eq(e.errorLabels, ["RetryableWriteError"], e);
    }
    retrySession.endSession();
}, st.s.host, dbName, collName);

insertThread.start();
hangBeforeCheckInterruptFailPoint.wait();

st.stopMongos(0);
insertThread.join();

st.stop();
})();
