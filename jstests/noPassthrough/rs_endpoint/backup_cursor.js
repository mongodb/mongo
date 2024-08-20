/*
 * Tests that the replica set endpoint supports $backupCursor and $backupCursorExtend.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagRouterPort,
 * ]
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (!(buildInfo().modules.includes("enterprise"))) {
    jsTest.log("Skipping test as it requires the enterprise module");
    quit();
}

/*
 * Tests that backup cursors cannot be created by running $backupCursor and $backupCursorExtend
 * aggregate commands against the 'dbName' database. Writes to the database and collection specified
 * in 'writeOptions' and drops the database at the end.
 */
function testBackupCursorNotSupported(routerHost, shard0PrimaryHost, dbName, writeOptions) {
    const router = new Mongo(routerHost);
    const shard0Primary = new Mongo(shard0PrimaryHost);
    jsTest.log(`Testing that $backupCursor and $backupCursorExtend are not supported against '${
        dbName}' database: ${tojsononeline({routerHost, shard0PrimaryHost, writeOptions})}`);

    const testDB = router.getDB(writeOptions.dbName);
    assert.commandWorked(router.adminCommand(
        {enableSharding: writeOptions.dbName, primaryShard: writeOptions.primaryShard}));
    assert.commandWorked(testDB.runCommand({
        insert: writeOptions.collName,
        documents: [{x: new Array(writeOptions.fieldSize).join("0")}]
    }));

    const routerDB = router.getDB(dbName);
    const shard0PrimaryDB = shard0Primary.getDB(dbName);

    jsTest.log(`Testing $backupCursor against '${dbName}' database`);
    const backupCursorCmdObj = {
        aggregate: 1,
        pipeline: [{$backupCursor: {}}],
        cursor: {},
    };
    assert.commandFailedWithCode(routerDB.runCommand(backupCursorCmdObj),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(shard0PrimaryDB.runCommand(backupCursorCmdObj),
                                 ErrorCodes.InvalidNamespace);

    jsTest.log(`Testing $backupCursorExtend against '${dbName}' database`);
    const backupCursorExtendCmdObj = {
        aggregate: 1,
        pipeline: [{$backupCursorExtend: {}}],
        cursor: {},
    };
    assert.commandFailedWithCode(routerDB.runCommand(backupCursorExtendCmdObj),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(shard0PrimaryDB.runCommand(backupCursorExtendCmdObj),
                                 ErrorCodes.InvalidNamespace);

    assert.commandWorked(testDB.runCommand({dropDatabase: 1}));
}

/*
 * Tests creating and using backup cursors created by running $backupCursor and $backupCursorExtend
 * aggregate commands against the 'dbName' database. Writes to the database and collection specified
 * in 'writeOptions' and drops the database at the end.
 */
function testBackupCursorSupported(routerHost, shard0PrimaryHost, dbName, writeOptions) {
    const router = new Mongo(routerHost);
    const shard0Primary = new Mongo(shard0PrimaryHost);
    jsTest.log(`Testing that $backupCursor and $backupCursorExtend are supported against '${
        dbName} database': ${tojsononeline({routerHost, shard0PrimaryHost, writeOptions})}`);

    const testDB = router.getDB(writeOptions.dbName);
    assert.commandWorked(router.adminCommand(
        {enableSharding: writeOptions.dbName, primaryShard: writeOptions.primaryShard}));
    assert.commandWorked(testDB.runCommand({
        insert: writeOptions.collName,
        documents: [{x: new Array(writeOptions.fieldSize).join("0")}]
    }));

    const routerDB = router.getDB(dbName);
    const shard0PrimaryDB = shard0Primary.getDB(dbName);
    const isLocalDB = dbName == "local";

    jsTest.log(`Testing $backupCursor against '${dbName}' database`);
    // By default, $backupCursor returns multiple docs.
    const backupCursorCmdObj = {
        aggregate: 1,
        pipeline: [{$backupCursor: {}}],
        cursor: {batchSize: 1},
    };
    assert.commandFailedWithCode(routerDB.runCommand(backupCursorCmdObj),
                                 // If the command is run against the 'local' database, the expected
                                 // error is IllegalOperation because it is illegal to run commands
                                 // against the 'local' database through a router.
                                 isLocalDB ? ErrorCodes.IllegalOperation : ErrorCodes.CannotBackup);
    const backupCursorRes = assert.commandWorked(shard0PrimaryDB.runCommand(backupCursorCmdObj));
    jsTest.log("Response for $backupCursor aggregate command: " + tojson(backupCursorRes));
    const backupMetadata = backupCursorRes.cursor.firstBatch[0].metadata;
    const backupCursorId = backupCursorRes.cursor.id;

    // Verify that there can only be at most one open backup cursor.
    assert.commandFailedWithCode(shard0PrimaryDB.runCommand(backupCursorCmdObj), 50886);

    {
        const getMoreCmdObj = {getMore: backupCursorId, collection: "$cmd.aggregate"};
        let lastGetMoreBatch;
        while (!lastGetMoreBatch || lastGetMoreBatch.length > 0) {
            const getMoreRes = assert.commandWorked(shard0PrimaryDB.runCommand(getMoreCmdObj));
            jsTest.log("Response for $backupCursor getMore command: " + tojson(getMoreRes));
            lastGetMoreBatch = getMoreRes.cursor.nextBatch;
            // The cursor for $backupCursor is tailable, so should remain open even after there are
            // more docs to return.
            assert.neq(getMoreRes.cursor.id, 0);
        }
    }

    // Verify that there can only be at most one open backup cursor (the exhausted cursor above
    // should still be there).
    assert.commandFailedWithCode(shard0PrimaryDB.runCommand(backupCursorCmdObj), 50886);

    jsTest.log(`Testing $backupCursorExtend against '${dbName}' database`);
    // Perform some large writes to make $backupCursorExtend return multiple docs.
    const docs = [];
    for (let i = 0; i < 10; i++) {
        docs.push({x: new Array(writeOptions.fieldSize).join(i.toString())});
    }
    const operationTime =
        assert.commandWorked(testDB.runCommand({insert: writeOptions.collName, documents: docs}))
            .operationTime;

    const backupCursorExtendCmdObj = {
        aggregate: 1,
        pipeline:
            [{$backupCursorExtend: {backupId: backupMetadata.backupId, timestamp: operationTime}}],
        cursor: {batchSize: 1},
    };
    assert.commandFailedWithCode(routerDB.runCommand(backupCursorExtendCmdObj),
                                 // If the command is run against the 'local' database, the expected
                                 // error is IllegalOperation because it is illegal to run commands
                                 // against the 'local' database through a router.
                                 isLocalDB ? ErrorCodes.IllegalOperation : ErrorCodes.CannotBackup);
    const backupCursorExtendRes0 =
        assert.commandWorked(shard0PrimaryDB.runCommand(backupCursorExtendCmdObj));
    jsTest.log("Response for the first $backupCursorExtend aggregate command: " +
               tojson(backupCursorExtendRes0));
    const backupCursorExtendId0 = backupCursorExtendRes0.cursor.id;

    // Verify that there can only be multiple open $backupCursorExtend cursors.
    const backupCursorExtendRes1 =
        assert.commandWorked(shard0PrimaryDB.runCommand(backupCursorExtendCmdObj));
    jsTest.log("Response for the second $backupCursorExtend aggregate command:  " +
               tojson(backupCursorExtendRes1));
    const backupCursorExtendId1 = backupCursorExtendRes1.cursor.id;

    {
        const getMoreCmdObj = {getMore: backupCursorExtendId0, collection: "$cmd.aggregate"};
        while (true) {
            const getMoreRes = assert.commandWorked(shard0PrimaryDB.runCommand(getMoreCmdObj));
            jsTest.log("Response for $backupCursorExtend getMore command: " + tojson(getMoreRes));
            assert.gt(getMoreRes.cursor.nextBatch.length, 0, getMoreRes);
            if (getMoreRes.cursor.id == 0) {
                // The cursor for $backupCursorExtend is not tailable, so should get closed after
                // there are more docs to return.
                break;
            }
        }
    }

    jsTest.log("Test killing cursors");
    const killCursorsRes0 = assert.commandWorked(
        shard0PrimaryDB.runCommand({killCursors: "$cmd.aggregate", cursors: [backupCursorId]}));
    // The cursor for $backupCursor should still be there.
    assert.sameMembers(killCursorsRes0.cursorsKilled, [backupCursorId], killCursorsRes0);
    const killCursorsRes1 = assert.commandWorked(shard0PrimaryDB.runCommand(
        {killCursors: "$cmd.aggregate", cursors: [backupCursorExtendId0, backupCursorExtendId1]}));
    // The first cursor for $backupCursorExtend should still not be there.
    assert.sameMembers(killCursorsRes1.cursorsNotFound, [backupCursorExtendId0], killCursorsRes1);
    // The second cursor for $backupCursorExtend should still be there.
    assert.sameMembers(killCursorsRes1.cursorsKilled, [backupCursorExtendId1], killCursorsRes1);

    assert.commandWorked(testDB.runCommand({dropDatabase: 1}));
}

function addShard(routerHost, shardName, shardURL) {
    const router = new Mongo(routerHost);
    assert.commandWorked(router.adminCommand({addShard: shardURL, name: shardName}));
}

function removeShard(routerHost, shardName) {
    const router = new Mongo(routerHost);
    assert.soon(() => {
        const res = assert.commandWorked(router.adminCommand({removeShard: shardName}));
        return res.state == "completed";
    });
}

const kTestDbName = "testDb";
const kTestCollName = "testColl";
// Set the WiredTiger file size and document field size such that the inserts above would generate
// multiple files.
const kWiredTigerEngineConfigString = "log=(file_max=100K)";
const kFieldSize100KB = 100 * 1024;
const kWriteOptions = {
    dbName: kTestDbName,
    collName: kTestCollName,
    fieldSize: kFieldSize100KB
};

function runTests(isReplicaSetEndpointEnabled) {
    const st = new ShardingTest({
        shards: 1,
        rs: {
            nodes: 1,
            setParameter: {featureFlagReplicaSetEndpoint: isReplicaSetEndpointEnabled},
        },
        rsOptions: {wiredTigerEngineConfigString: kWiredTigerEngineConfigString},
        configShard: true,
        embeddedRouter: true
    });
    const shard0Primary = st.rs0.getPrimary();
    const routerHost = shard0Primary.routerHost;
    const shard0PrimaryHost = shard0Primary.host;
    const shard0Name = st.shard0.shardName;

    const writeOptions = Object.assign({primaryShard: shard0Name}, kWriteOptions);

    jsTest.log(`Testing $backupCursor and $backupCursorExtend basics ${
        tojson({isReplicaSetEndpointEnabled})}`);

    // $backupCursor and $backupCursorExtend are supported against 'local' database whether or not
    // the replica set endpoint feature flag is enabled.
    const testBackupCursorLocalDB = testBackupCursorSupported;
    testBackupCursorLocalDB(routerHost, shard0PrimaryHost, "local", writeOptions);

    // $backupCursor and $backupCursorExtend are only supported against non 'local' databases when
    // the replica set endpoint feature flag is not enabled.
    const testBackupCursorNonLocalDB =
        isReplicaSetEndpointEnabled ? testBackupCursorNotSupported : testBackupCursorSupported;
    testBackupCursorNonLocalDB(routerHost, shard0PrimaryHost, "admin", writeOptions);
    testBackupCursorNonLocalDB(routerHost, shard0PrimaryHost, "config", writeOptions);
    testBackupCursorNonLocalDB(routerHost, shard0PrimaryHost, writeOptions.dbName, writeOptions);
    // Non-existent database.
    testBackupCursorNonLocalDB(routerHost, shard0PrimaryHost, "otherDb", writeOptions);

    const shard1Name = "shard1";
    const shard1Rst = new ReplSetTest({name: shard1Name, nodes: 2});
    shard1Rst.startSet({shardsvr: ""});
    shard1Rst.initiate();

    jsTest.log("Testing $backupCursor and $backupCursorExtend with concurrent addShard");
    // When the replica set endpoint feature flag is enabled, adding a second shard makes the
    // replica set endpoint become inactive.
    const addShardThread = new Thread(addShard, routerHost, shard1Name, shard1Rst.getURL());
    const backupCursorThread0 =
        new Thread(testBackupCursorSupported, routerHost, shard0PrimaryHost, "local", writeOptions);

    addShardThread.start();
    backupCursorThread0.start();
    addShardThread.join();
    backupCursorThread0.join();

    jsTest.log("Testing $backupCursor and $backupCursorExtend with concurrent removeShard");
    // When the replica set endpoint feature flag is enabled, removing the second shard makes the
    // replica set endpoint become active again.
    const removeShardThread = new Thread(removeShard, routerHost, shard1Name, shard1Rst.getURL());
    const backupCursorThread1 =
        new Thread(testBackupCursorSupported, routerHost, shard0PrimaryHost, "local", writeOptions);

    removeShardThread.start();
    backupCursorThread1.start();
    removeShardThread.join();
    backupCursorThread1.join();

    shard1Rst.stopSet();
    st.stop();
}

jsTest.log("Testing $backupCursor and $backupCursorExtend in a sharded cluster with replica " +
           "set endpoint");
runTests(true /* isReplicaSetEndpointEnabled */);

jsTest.log("Testing $backupCursor and $backupCursorExtend in a sharded cluster without replica " +
           "set endpoint to verify that the behavior is the same");
runTests(false /* isReplicaSetEndpointEnabled */);
