/**
 * Helper function to move unsharded, tracked collections as well as unsharded, untracked
 * collections and the database primary for a given database.
 */
// TODO (SERVER-87807): Issue moveCollection for untracked collections
export function moveDatabaseAndUnshardedColls(db, destinationShard) {
    let configDB = db.getSiblingDB('config');
    const originShard = configDB.getCollection('databases').findOne({_id: db.getName()}).primary;
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
    // Always call movePrimary at the end. This will move all untracked collections and change the
    // db metadata.
    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: destinationShard}));
}
