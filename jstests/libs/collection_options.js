/**
 * Asserts that the given collection option is set to true.
 */
export function assertCollectionOptionIsEnabled(db, collName, option) {
    const collectionInfos = db.getCollectionInfos({name: collName});
    assert(collectionInfos[0].options[option] === true);
}

/**
 * Asserts that the given collection option is absent.
 */
export function assertCollectionOptionIsAbsent(db, collName, option) {
    const collectionInfos = db.getCollectionInfos({name: collName});
    assert(!collectionInfos[0].options.hasOwnProperty(option));
}
