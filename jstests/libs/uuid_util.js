//  Utilities for testing UUIDs.

/**
 * Reads the collection entry for 'nss' from config.collections, asserts that such an entry exists,
 * and returns its 'uuid' field, which may be undefined.
 */
function getUUIDFromConfigCollections(mongosConn, nss) {
    let collEntry = mongosConn.getDB("config").collections.findOne({_id: nss});
    assert.neq(undefined, collEntry);
    return collEntry.uuid;
}

/**
 * Calls listCollections on a connection to 'db' with a filter for 'collName', asserts that a result
 * for 'collName' was returned, and returns its 'uuid' field, which may be undefined.
 */
function getUUIDFromListCollections(db, collName) {
    let listCollsRes = db.runCommand({listCollections: 1, filter: {name: collName}});
    assert.commandWorked(listCollsRes);
    assert.neq(undefined, listCollsRes.cursor);
    assert.neq(undefined, listCollsRes.cursor.firstBatch);
    assert.eq(1, listCollsRes.cursor.firstBatch.length);
    assert.neq(undefined, listCollsRes.cursor.firstBatch[0].info);
    return listCollsRes.cursor.firstBatch[0].info.uuid;
}
