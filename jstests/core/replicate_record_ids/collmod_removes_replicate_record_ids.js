/**
 * Tests that we can remove the 'recordIdsReplicated' flag from
 * a collection's catalog entry via the collMod command.
 *
 * This allows us to irreversibly disable replicated record IDs on a collection.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   assumes_no_implicit_collection_creation_after_drop,  # common tag in collMod tests.
 *   expects_explicit_underscore_id_index,
 *   requires_non_retryable_commands, # common tag in collMod tests.
 *   featureFlagRecordIdsReplicated,
 *   # TODO (SERVER-118693): Check if this tag can be removed.
 *   # Unsetting 'recordIdsReplicated' option with the collMod command can create a test-only race condition during initial sync.
 *   incompatible_with_initial_sync,
 * ]
 */

const collName = "replRecIdCollForCollMod";
const coll = db.getCollection(collName);
coll.drop();

// Create a collection with the param set.
jsTestLog("Creating collection " + coll.getFullName());
assert.commandWorked(db.runCommand({create: collName, recordIdsReplicated: true}));
jsTestLog("Created collection " + coll.getFullName());

let collInfo = coll.exists();
assert(
    collInfo,
    "unable to find collection " +
        coll.getFullName() +
        " in listCollections results after creating collection: " +
        tojson(db.getCollectionInfos()),
);
jsTestLog("Collection options after creation: " + tojson(collInfo));
assert(
    collInfo.options.hasOwnProperty("recordIdsReplicated"),
    "collection options does not contain recordIdsReplicated flag after collection creation",
);

jsTestLog("Inserting document into " + coll.getFullName());
assert.commandWorked(coll.insert({_id: 1}));
jsTestLog("Inserted document into " + coll.getFullName());

jsTestLog("Running collMod command to remove recordIdsReplication collection option from " + coll.getFullName());
let result = assert.commandWorked(db.runCommand({collMod: collName, recordIdsReplicated: false}));
jsTestLog("Result from successful collMod command: " + tojson(result));

// Confirm that 'recordIdsReplicated' option has been removed from collection options.
collInfo = coll.exists();
assert(
    collInfo,
    "unable to find collection " +
        coll.getFullName() +
        " in listCollections results after running colMod: " +
        tojson(db.getCollectionInfos()),
);
jsTestLog("Collection options after collMod: " + tojson(collInfo));
assert(
    !collInfo.options.hasOwnProperty("recordIdsReplicated"),
    "collMod failed to remove recordIdsReplicated flag from collection options",
);

// Running collMod to unset 'recordIdsReplicated' on a collection that does not replicate record IDs is allowed.
assert.commandWorked(db.runCommand({collMod: collName, recordIdsReplicated: false}));

// Modifying with a true 'recordIdsReplicated' value is not allowed
assert.commandFailedWithCode(db.runCommand({collMod: collName, recordIdsReplicated: true}), ErrorCodes.InvalidOptions);
