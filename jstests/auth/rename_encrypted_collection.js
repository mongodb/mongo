/**
 * Verify renaming to/from a collection with encrypted fields is disallowed depending on the users
 * privileges
 *
 * @tags: [
 * requires_fcv_61,
 * ]
 */
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTestWithAuth(conn, allowsRename, verifyFunction) {
    const srcDbName = 'rename_encrypted_collection_src_db';
    const tgtDbName = 'rename_encrypted_collection_tgt_db';

    const srcDbClient = new EncryptedClient(conn, srcDbName);
    const tgtDbClient = new EncryptedClient(conn, tgtDbName);

    const dbSrc = srcDbClient.getDB();
    const dbTgt = tgtDbClient.getDB();

    dbSrc.encrypted.drop();
    dbTgt.encrypted.drop();

    const sampleEncryptedFields = {
        "fields": [
            {
                "path": "firstName",
                "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                "bsonType": "string",
                "queries": {"queryType": "equality"}
            },
        ]
    };

    const srcEncryptedErrmsg = "Cannot rename an encrypted collection";
    const tgtEncryptedErrmsg = "Cannot rename to an existing encrypted collection";

    assert.commandWorked(srcDbClient.createEncryptionCollection(
        "encrypted", {encryptedFields: sampleEncryptedFields}));
    assert.commandWorked(dbSrc.createCollection("unencrypted"));

    assert.commandWorked(tgtDbClient.createEncryptionCollection(
        "encrypted", {encryptedFields: sampleEncryptedFields}));

    jsTestLog("Test renaming encrypted collection to another namespace is prohibited");
    verifyFunction(
        dbSrc.adminCommand({renameCollection: dbSrc + ".encrypted", to: dbSrc + ".renamed"}),
        "Renaming an encrypted collection within same DB passed",
        srcEncryptedErrmsg);

    if (!allowsRename) {
        verifyFunction(dbSrc.adminCommand(
                           {renameCollection: dbSrc + ".encrypted", to: dbTgt + ".unencrypted"}),
                       "Renaming an encrypted collection across DBs passed",
                       srcEncryptedErrmsg);
    }

    jsTestLog("Test renaming unencrypted collection to an encrypted namespace is prohibited");
    verifyFunction(
        dbSrc.adminCommand(
            {renameCollection: dbSrc + ".unencrypted", to: dbSrc + ".encrypted", dropTarget: true}),
        "Renaming to an encrypted collection within same DB passed",
        tgtEncryptedErrmsg);

    if (!allowsRename) {
        verifyFunction(dbSrc.adminCommand({
            renameCollection: dbSrc + ".unencrypted",
            to: dbTgt + ".encrypted",
            dropTarget: true
        }),
                       "Renaming to an encrypted collection across DBs passed",
                       tgtEncryptedErrmsg);
    }
}

function runTest(conn) {
    const adminDB = conn.getDB("admin");

    // Create the admin user.
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    assert.eq(1, adminDB.auth("admin", "admin"));

    // Create a low priv user
    assert.commandWorked(adminDB.runCommand(
        {createUser: "lowpriv", pwd: "lowpriv", roles: ["readWriteAnyDatabase"]}));

    // Run tests with a user that has restore/backup role and verify they can rename
    runTestWithAuth(conn, true, (cmdObj, assertMsg, errorMsg) => {
        assert.commandWorked(cmdObj, assertMsg);
    });
    adminDB.logout();

    assert.eq(1, adminDB.auth("lowpriv", "lowpriv"));

    // Run tests with a user that does not have restore/backup and verify the rename fails
    runTestWithAuth(conn, false, (cmd, assertMsg, errorMsg) => {
        let res = assert.commandFailedWithCode(cmd, ErrorCodes.IllegalOperation, assertMsg);
        assert.eq(res.errmsg, errorMsg);
    });
}

if (!isEnterpriseShell()) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

jsTestLog("ReplicaSet: Testing fle2 collection rename");
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet({auth: "", keyFile: 'jstests/libs/key1'});

    rst.initiate();
    rst.awaitReplication();
    runTest(rst.getPrimary(), rst.getPrimary());
    rst.stopSet();
}

jsTestLog("Sharding: Testing fle2 collection rename");
{
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        keyFile: "jstests/libs/key1",
        other: {rsOptions: {auth: ""}}
    });

    runTest(st.s);

    st.stop();
}
