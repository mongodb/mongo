/**
 * Tests create options for collections with {recordIdsReplicated: true}.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   expects_explicit_underscore_id_index,
 *   featureFlagRecordIdsReplicated,
 *   # hasRecordIdsReplicated uses $_internalListCollections which requires the __system role.
 *   auth_incompatible,
 *   # hasRecordIdsReplicated uses $_internalListCollections which only supports local read concern.
 *   assumes_read_concern_unchanged,
 * ]
 */

import {hasRecordIdsReplicated} from "jstests/libs/collection_write_path/replicated_record_ids_utils.js";

const collName = "replRecIdCollForCreate";
const coll = db.getCollection(collName);
coll.drop();

// Validate that a clustered collection will never have the 'recordIdsReplicated' option set
// implicitly.
const clusteredCollName = collName + "_clustered";
assert.commandWorked(db.createCollection(clusteredCollName, {clusteredIndex: {key: {_id: 1}, unique: true}}));
assert(
    !hasRecordIdsReplicated(db, clusteredCollName),
    "clustered collection created with recordIdsReplicated collection option",
);

// Create a collection with the param set.
jsTestLog("Creating collection " + coll.getFullName());
assert.commandWorked(db.runCommand({create: collName}));
jsTestLog("Created collection " + coll.getFullName());

// Check collections options in listCollections output.
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
    hasRecordIdsReplicated(db, collName),
    "collection options does not contain recordIdsReplicated flag after collection creation",
);
