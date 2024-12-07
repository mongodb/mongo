/*
 * Test topology-agnostic scenarios for renameCollection.
 * @tags: [
 *   # This test covers the behaviour of untracked and non-existing collections in a sharded cluster
 *   assumes_unsharded_collection,
 *   assumes_no_implicit_collection_creation_after_drop,
 * ]
 */

import {setupTestDatabase} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

const dbName = jsTestName();

{
    const testDB = setupTestDatabase(db, dbName);
    jsTest.log(
        'Testing unshardedColl.renameCollection to a sharded collection without dropTarget=true');
    assert.commandWorked(
        testDB.adminCommand({shardCollection: `${dbName}.shardedColl`, key: {_id: 'hashed'}}));

    assert.commandWorked(testDB.unshardedColl.insert({_id: 1}));
    assert.commandFailed(testDB.unshardedColl.renameCollection('shardedColl'));
}

{
    jsTest.log('Testing renameCollection against forbidden internal namespaces');
    const testDB = setupTestDatabase(db, dbName);
    function assertRenameFailed(dbName, fromCollName) {
        const fromColl = testDB.getSiblingDB(dbName).getCollection(fromCollName);
        assert.commandFailedWithCode(fromColl.renameCollection('new'), ErrorCodes.IllegalOperation);
    }

    assertRenameFailed('config', 'shards');
    assertRenameFailed('config', 'inexistent');

    assertRenameFailed('admin', 'system.version');
    assertRenameFailed('admin', 'inexistent');
}

{
    jsTest.log(
        'Testing renameCollection to existing sharded target collection with dropTarget=true');
    const dbName = 'testRenameToExistingShardedCollection';
    const testDB = setupTestDatabase(db, dbName);
    const fromCollName = 'fromColl';
    const targetCollName = 'targetColl';

    assert.commandWorked(
        testDB.adminCommand({shardCollection: `${dbName}.${fromCollName}`, key: {_id: 'hashed'}}));
    assert.commandWorked(testDB[fromCollName].insertOne({sentinel: 'createdWithinFromColl'}));

    assert.commandWorked(testDB.adminCommand(
        {shardCollection: `${dbName}.${targetCollName}`, key: {_id: 'hashed'}}));
    assert.commandWorked(testDB[targetCollName].insertOne({sentinel: 'createdWithinTargetColl'}));

    assert.commandWorked(
        testDB[fromCollName].renameCollection(targetCollName, true /* dropTarget */));

    assert.eq(1, testDB[targetCollName].countDocuments({sentinel: 'createdWithinFromColl'}));
    assert.eq(0, testDB[targetCollName].countDocuments({sentinel: 'createdWithinTargetColl'}));
}