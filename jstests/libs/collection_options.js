/**
 * Asserts that the given collection option is set to true.
 */
function assertCollectionOptionIsEnabled(db, collName, option) {
    const collectionInfos = db.getCollectionInfos({name: collName});
    assert(collectionInfos[0].options[option] === true);
}

/**
 * Asserts that the given collection option is absent.
 */
function assertCollectionOptionIsAbsent(db, collName, option) {
    const collectionInfos = db.getCollectionInfos({name: collName});
    assert(!collectionInfos[0].options.hasOwnProperty(option));
}
