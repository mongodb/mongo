/**
 * Attempts to drop the given collection and asserts either that the drop succeeded or the
 * collection did not exist. Avoids automatically recreating the collection in the case of test
 * suites where accessing or dropping the collection implicitly recreates it.
 */
export function assertDropCollection(db, collName) {
    let cmdRes = db.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert(cmdRes.ok === 1 || cmdRes.code === ErrorCodes.NamespaceNotFound, tojson(cmdRes));
}

/**
 * Attempts to create a collection with the given name and options, if any, and asserts on failure.
 * Returns the newly-created collection on success. When running under a sharded collections
 * passthrough, the new collection will be implicitly sharded.
 */
export function assertCreateCollection(db, collName, collOpts) {
    assert.commandWorked(db.createCollection(collName, collOpts));
    return db.getCollection(collName);
}

/**
 * Attempts to drop a collection with the given name and recreate it with the specified options, if
 * any. Asserts if either step fails. Returns the newly-created collection on success. When running
 * under a sharded collections passthrough, the new collection will be implicitly sharded.
 */
export function assertDropAndRecreateCollection(db, collName, collOpts) {
    assertDropCollection(db, collName);
    return assertCreateCollection(db, collName, collOpts);
}
