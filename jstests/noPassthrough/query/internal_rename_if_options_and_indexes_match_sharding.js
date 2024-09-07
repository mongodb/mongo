/*
 * Tests the internalRenameIfOptionsAndIndexesMatch on shard servers.
 * Note: Because the mongos doesn't expose this command, we need a directed noPassthrough test to be
 * able to test it by running it directly on shards.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const dbName = "test";
const testDB = st.getDB(dbName);
const sourceColl = testDB['source'];
const destColl = testDB['dest'];
const inexistentColl = testDB['inexistentColl'];

function setupCollections() {
    sourceColl.drop();
    destColl.drop();
    inexistentColl.drop();

    sourceColl.insert({x: 1});
    destColl.insert({});
}

setupCollections();

const dbPrimaryShardConn = st.getPrimaryShard(dbName);

function makeCorrectCommand(sourceColl, destColl) {
    const dbVersion = st.s.getDB('config')['databases'].findOne({_id: dbName}).version;

    let collectionInfos = testDB.getCollectionInfos({name: destColl.getName()});
    let collectionOptions = collectionInfos.length === 1 ? collectionInfos[0].options : {};

    return {
        internalRenameIfOptionsAndIndexesMatch: 1,
        from: sourceColl.getFullName(),
        to: destColl.getFullName(),
        indexes: destColl.getIndexes(),
        collectionOptions: collectionOptions,
        databaseVersion: dbVersion
    };
}

// If no dbVersion attached, command fails.
{
    let cmd = makeCorrectCommand(sourceColl, destColl);
    delete cmd.databaseVersion;
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd), ErrorCodes.IllegalOperation);
}

// If wrong dbVersion attached, command fails
{
    let cmd = makeCorrectCommand(sourceColl, destColl);
    cmd.databaseVersion.timestamp = Timestamp(12345, 6789);
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd), ErrorCodes.StaleDbVersion);
}

// If source collection does not exist, command fails.
{
    let cmd = makeCorrectCommand(inexistentColl, destColl);
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd),
                                 ErrorCodes.NamespaceNotFound);
}

// If expected indexes don't match, command fails.
{
    destColl.createIndex({x: 1});

    // Collection does not exist, but indexes expected.
    let cmd = makeCorrectCommand(sourceColl, inexistentColl);
    cmd.indexes = destColl.getIndexes();
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd), ErrorCodes.CommandFailed);

    // Collection exists and has indexes, but wrong indexes expected.
    cmd = makeCorrectCommand(sourceColl, destColl);
    destColl.dropIndex("x_1");
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd), ErrorCodes.CommandFailed);
}
setupCollections();

// If expected options don't match, command fails.
{
    assert.commandWorked(testDB.runCommand({collMod: destColl.getName(), validator: {a: 1}}));
    const options = testDB.getCollectionInfos({name: destColl.getName()})[0];

    // Collection does not exist, but some options expected.
    let cmd = makeCorrectCommand(sourceColl, inexistentColl);
    cmd.collectionOptions = options;
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd), ErrorCodes.CommandFailed);

    // Collection exists and has options set, but wrong options expected.
    cmd = makeCorrectCommand(sourceColl, destColl);
    assert.commandWorked(testDB.runCommand({collMod: destColl.getName(), validator: {}}));
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd), ErrorCodes.CommandFailed);
}
setupCollections();

// If target collection is sharded, fails
{
    let shardedColl = testDB['sharded'];
    assert.commandWorked(
        st.s.adminCommand({shardCollection: shardedColl.getFullName(), key: {x: 1}}));
    let cmd = makeCorrectCommand(sourceColl, shardedColl);
    assert.commandFailedWithCode(dbPrimaryShardConn.adminCommand(cmd), ErrorCodes.IllegalOperation);
}
setupCollections();

// Expectations are met. Command works.
{
    let cmd = makeCorrectCommand(sourceColl, destColl);
    assert.commandWorked(dbPrimaryShardConn.adminCommand(cmd));
    assert.eq(1, destColl.find({x: 1}).itcount());

    setupCollections();
    cmd = makeCorrectCommand(sourceColl, inexistentColl);
    assert.commandWorked(dbPrimaryShardConn.adminCommand(cmd));
    assert.eq(1, inexistentColl.find({x: 1}).itcount());
}

st.stop();
