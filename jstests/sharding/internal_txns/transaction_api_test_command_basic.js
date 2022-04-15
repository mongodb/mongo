/*
 * Basic tests confirming functionality of the interalTransactionsTestCommand.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
'use strict';

// This test intentionally runs commands without a logical session id, which is not compatible
// with implicit sessions.
TestData.disableImplicitSessions = true;

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = kDbName + "." + kCollName;

const st = new ShardingTest({shards: 1});
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let db = primary.getDB(kDbName);
let rstColl = db.getCollection(kCollName);

function runTxn(connection, commandInfos, collection) {
    const res = assert.commandWorked(
        connection.adminCommand({testInternalTransactions: 1, commandInfos: commandInfos}));
    jsTest.log(res);

    let i = 0;
    commandInfos.forEach(commandInfo => {
        assert.eq(1, res.responses[i].ok);
        assert.eq(commandInfo.command.documents.length, res.responses[i].n);
        commandInfo.command.documents.forEach(document => {
            assert.eq(document, collection.findOne(document));
        });
        ++i;
    });
}

// Insert initial data.
assert.commandWorked(st.s.getCollection(kNs).insert([{_id: 0}]));
assert.commandWorked(rstColl.insert([{_id: 0}]));

const commandInfos0 = [{
    dbName: kDbName,
    command: {
        insert: kCollName,
        documents: [{_id: 1}],
    }
}];

const commandInfos1 = [
    {
        dbName: kDbName,
        command: {
            insert: kCollName,
            documents: [{_id: 2}],
        }
    },
    {
        dbName: kDbName,
        command: {
            insert: kCollName,
            documents: [{_id: 3}, {_id: 4}],
        }
    }
];

jsTest.log(
    "Insert documents without a session into a sharded cluster, using internal transactions test command.");
runTxn(st.s, commandInfos0, st.s.getCollection(kNs));
runTxn(st.s, commandInfos1, st.s.getCollection(kNs));

jsTest.log(
    "Insert documents without a session into a replica set, using internal transactions test command.");
runTxn(primary, commandInfos0, rstColl);
runTxn(primary, commandInfos1, rstColl);

// TODO SERVER-65048: Add testing for retryable writes and txns run in sessions.

rst.stopSet();
st.stop();
})();
