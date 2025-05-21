/*
 Ensure that dropCollection may be correctly executed to drop <dbName>.system.profile collections
 through a mongos connection.
 * @tags: [
 *   # The test creates and drops a single non-replicated collection, non shard-able collection
 *   assumes_unsharded_collection,
 *   assumes_read_preference_unchanged,
 * ]
 */
const profileCollName = "system.profile";

assert.commandWorked(db.dropDatabase());

assert.commandWorked(db.createCollection(profileCollName, {
    capped: true,
    size: 1024,
}));

assert.commandWorked(db.runCommand({drop: profileCollName}));
