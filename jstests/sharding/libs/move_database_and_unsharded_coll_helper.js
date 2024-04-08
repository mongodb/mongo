/**
 * Helper function to move unsharded, tracked collections as well as unsharded, untracked
 * collections and the database primary for a given database.
 */
// TODO (SERVER-87807): Issue moveCollection for untracked collections
export function moveDatabaseAndUnshardedColls(db, destinationShard) {
    let configDB = db.getSiblingDB('config');
    const originShard = configDB.getCollection('databases').findOne({_id: db.getName()}).primary;
    // First, move all tracked collections.
    configDB.getCollection('collections')
        .find({_id: {$regex: '^' + db.getName() + '\..*'}, unsplittable: true})
        .forEach(coll => {
            configDB.getCollection('chunks')
                .find({uuid: coll.uuid, shard: originShard})
                .forEach(() => {
                    // Move unsplittable collection data.
                    assert.commandWorked(
                        db.adminCommand({moveCollection: coll._id, toShard: destinationShard}));
                });
        });
    // Next, move all untracked collections via moveCollection. This can fail with InvalidOptions if
    // featureFlagMoveCollection is not enabled, IllegalOperation if the collection is not allowed
    // to be tracked, NamespaceNotFound if featureFlagTrackUponCreate is enabled but the
    // collection is untrackable, or CommandNotFound if the moveCollection command is unsupported in
    // multiversion scenarios.
    // TODO (SERVER-86443) remove this whole section once all collections are tracked.
    let listCollectionsCursor = db.runCommand({listCollections: 1});
    let localCollections = new DBCommandCursor(db, listCollectionsCursor).toArray();
    localCollections.forEach(coll => {
        let ns = db.getName() + '.' + coll.name;
        if (configDB.getCollection('collections').countDocuments({_id: ns}) == 0) {
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({moveCollection: ns, toShard: destinationShard}), [
                    ErrorCodes.InvalidOptions,
                    ErrorCodes.IllegalOperation,
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.CommandNotFound
                ]);
        }
    });
    // Always call movePrimary at the end. This will move all untracked collections and change the
    // db metadata.
    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: destinationShard}));
}
