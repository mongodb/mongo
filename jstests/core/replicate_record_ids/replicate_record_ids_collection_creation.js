/**
 * Tests create options for collections with {recordIdsReplicated: true}.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   expects_explicit_underscore_id_index,
 *   featureFlagRecordIdsReplicated,
 * ]
 */

const collName = 'replRecIdCollForCreate';
const coll = db.getCollection(collName);
coll.drop();

// Validate that it's not possible to create a clustered collection with
// {recordIdsReplicated: true}.
assert.commandFailedWithCode(
    db.createCollection(collName,
                        {clusteredIndex: {key: {_id: 1}, unique: true}, recordIdsReplicated: true}),
    ErrorCodes.InvalidOptions);

// Validate that a clustered collection will never have the 'recordIdsReplicated' option set
// implicitly.
const clusteredCollName = collName + '_clustered';
assert.commandWorked(
    db.createCollection(clusteredCollName, {clusteredIndex: {key: {_id: 1}, unique: true}}));
const clusteredCollInfo = db[clusteredCollName].exists();
assert(!clusteredCollInfo.options.hasOwnProperty('recordIdsReplicated'),
       'clustered collection created with recordIdsReplicated collection option: ' +
           tojson(clusteredCollInfo));

// Create a collection with the param set.
jsTestLog('Creating collection ' + coll.getFullName());
assert.commandWorked(db.runCommand({create: collName, recordIdsReplicated: true}));
jsTestLog('Created collection ' + coll.getFullName());

// Check collections options in listCollections output.
let collInfo = coll.exists();
assert(collInfo,
       'unable to find collection ' + coll.getFullName() +
           ' in listCollections results after creating collection: ' +
           tojson(db.getCollectionInfos()));
jsTestLog('Collection options after creation: ' + tojson(collInfo));
assert(collInfo.options.hasOwnProperty('recordIdsReplicated'),
       'collection options does not contain recordIdsReplicated flag after collection creation');
