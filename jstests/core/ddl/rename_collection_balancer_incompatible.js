/**
 * Includes topology-agnostic test cases that are not compatible with a balancer running in the
 * background.
 *
 * @tags: [
 *  #  It uses rename command that is not retriable.
 *  # After succeeding, any subsequent attempt will fail
 *  # because the source namespace does not exist anymore.
 *  requires_non_retryable_commands,
 *  assumes_balancer_off
 *  ]
 */

{
    jsTest.log('[C2C] Rename of existing collection with extra UUID parameter must succeed');
    const dbName = 'testRenameCollectionWithSourceUUID';
    const testDB = db.getSiblingDB(dbName);
    const fromCollName = 'from';
    const toCollName = 'to';
    assert.commandWorked(testDB.dropDatabase());

    const fromColl = testDB.getCollection(fromCollName);
    fromColl.insert({a: 0});

    const toColl = testDB.getCollection(toCollName);
    toColl.insert({b: 0});

    const sourceUUID = assert.commandWorked(testDB.runCommand({listCollections: 1}))
                           .cursor.firstBatch.find(c => c.name === fromCollName)
                           .info.uuid;

    // The command succeeds when the correct UUID is provided.
    assert.commandWorked(testDB.adminCommand({
        renameCollection: fromColl.getFullName(),
        to: toColl.getFullName(),
        dropTarget: true,
        collectionUUID: sourceUUID,
    }));
}
