/**
 * Reproduces the scenario described in SERVER-95730 where a single applyOps entry will contain
 * writes for multiple namespaces. This behavior is triggered by performing a bulkWrite on multiple
 * namespaces against a sharded cluster in a retryable write.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

const dbName = "test";
const collName1 = "foo";
const collName2 = "bar";

const coll1 = st.s.getDB(dbName)[collName1];
const coll2 = st.s.getDB(dbName)[collName2];

const session = st.s.startSession({retryWrites: true});
const lsid = session.getSessionId();

// The first insert to test.foo will setup the TransactionParticipant state with
// affectedCollections={test.foo}.
// The second and third inserts (which the shard will group together in one single applyOps oplog
// entry) will add test.bar to TransactionParticipant's affected collections (in addition to
// test.foo that was already there). Then MigrationChunkClonerSourceOpObserver::onBatchedWriteCommit
// will see both as affected namespaces. Us not hitting the dassert proves that we fixed the
// original issue.
assert.commandWorked(st.s.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {x: 0}},
        {insert: 1, document: {x: 1}},
        {insert: 1, document: {x: 2}}
    ],
    nsInfo: [{ns: coll1.getFullName()}, {ns: coll2.getFullName()}],
    lsid: lsid,
    txnNumber: NumberLong(0),
}));

st.stop();
