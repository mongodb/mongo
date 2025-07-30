/**
 * Test that the coordinateCommitTransaction command can only be run when
 * authorized to do so.
 *
 */
(function() {
"use strict";

const keyFile = 'jstests/libs/key1';
const collName = 'mycoll';
const id = UUID("eeeeeeee-eeee-1111-0000-eeeeeeeeeeee");

/**
 * userConnection represent the connection that a user has to the cluster.
 * directConnection represents the connection the malicious user has, which
 * is directly to a shard in a sharded cluster.
 * Both connections are the same when run on a replica set.
 */
function testCoordinateCommitTransactionFails(userConnection, directConnection) {
    jsTestLog("Create a 'good' user who is trying to run a transaction via mongos.");
    const userTestDB = userConnection.getDB('test');
    authutil.asCluster(userConnection, keyFile, () => {
        userTestDB.getSiblingDB("admin").createUser({user: 'root', pwd: 'root', roles: ['root']});
        userTestDB.getSiblingDB("admin").createUser({user: 'ro', pwd: 'ro', roles: ['read']});
    });
    assert.eq(1, userTestDB.getSiblingDB("admin").auth('root', 'root'));

    // In the replica set case, since both connections are the same, we've already
    // created the users. So we don't create the users again.
    if (userConnection != directConnection) {
        // The root user is to simulate someone who leaks the LSID.
        jsTestLog("Create root user and malicious read-only user on the shard.");
        const testDB = directConnection.getDB('test');
        authutil.asCluster(directConnection, keyFile, () => {
            testDB.getSiblingDB("admin").createUser({user: 'ro', pwd: 'ro', roles: ['read']});
            testDB.getSiblingDB("admin").createUser({user: 'root', pwd: 'root', roles: ['root']});
        });
    }

    jsTestLog("A user with read-only permissions can't run coordinateCommitTransaction:");
    const connRO = new Mongo(directConnection.host);
    const testDBRO = connRO.getDB('test');
    assert.eq(1, testDBRO.getSiblingDB("admin").auth('ro', 'ro'));
    assert.commandFailedWithCode(testDBRO.adminCommand({
        coordinateCommitTransaction: 1,
        participants: [],
    }),
                                 ErrorCodes.Unauthorized);

    // Insert a document and create the collection because we can't create collections in a
    // cross-shard transaction.
    assert.commandWorked(userTestDB[collName].insert({a: -1}));
    // Next we run one transaction to completion. This is so that config.transactions gets
    // populated. From the config.transactions entry we will figure out what the uid component of
    // the lsid is.
    assert.commandWorked(userTestDB.runCommand({
        insert: collName,
        documents: [{a: 0}],
        startTransaction: true,
        lsid: {"id": id},
        txnNumber: NumberLong(0),
        autocommit: false,
    }));
    assert.commandWorked(userTestDB.adminCommand({
        commitTransaction: 1,
        lsid: {"id": id},
        txnNumber: NumberLong(0),
        autocommit: false,
    }));

    // Now we start a second transaction, which we will attempt to commit through the malicious user
    // via the coordinateCommitTransaction command.
    jsTestLog("Good user starts a second transaction:");
    assert.commandWorked(userTestDB.runCommand({
        insert: collName,
        documents: [{a: 1}],
        startTransaction: true,
        lsid: {"id": id},
        txnNumber: NumberLong(1),
        autocommit: false,
    }));

    jsTestLog("Trying to get the uid of user.");
    const directRootConn = new Mongo(directConnection.host);
    const directRootTestDB = directRootConn.getDB('test');
    assert.eq(1, directRootTestDB.getSiblingDB("admin").auth('root', 'root'));

    let txnEntry = directRootTestDB.getSiblingDB('config').transactions.findOne({"_id.id": id});
    jsTestLog("config.transactions entry: " + tojson(txnEntry));
    assert(txnEntry._id.uid);

    jsTestLog("Sending coordinateCommitTransaction when impersonating lsid (id+uid) should fail:");
    const readOnlyConn = new Mongo(directConnection.host);
    const readOnlyTestDB = readOnlyConn.getDB('test');
    assert.eq(1, readOnlyTestDB.getSiblingDB("admin").auth('ro', 'ro'));
    const res = assert.commandFailedWithCode(readOnlyTestDB.adminCommand({
        coordinateCommitTransaction: 1,
        lsid: {
            id: id,
            uid: txnEntry._id.uid,
        },
        txnNumber: NumberLong(1),
        autocommit: false,
        participants: [],
    }),
                                             ErrorCodes.Unauthorized);
    assert(res.errmsg.includes("Unauthorized to set user digest"), res);
}

jsTestLog("Running test on replica set.");
const rs = new ReplSetTest({
    nodes: 1,
    setParameter: {logComponentVerbosity: tojsononeline({transaction: 4})},
    keyFile: keyFile
});
rs.startSet();
rs.initiate();
testCoordinateCommitTransactionFails(rs.getPrimary(), rs.getPrimary());
rs.stopSet();

jsTestLog("Running test on sharded cluster.");
const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 1, setParameter: {logComponentVerbosity: tojsononeline({transaction: 4})}},
    keyFile: keyFile,
});
testCoordinateCommitTransactionFails(st.s, st._connections[0]);
st.stop();
})();
