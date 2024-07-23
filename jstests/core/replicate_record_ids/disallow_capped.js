/**
 * Tests that capped collections can't be created with recordIdsReplicated:true. Also tests
 * that running 'cloneCollectionAsCapped' and 'convertToCapped' strip the collection of
 * the recordIdsReplicated:true property.
 *
 * @tags: [
 *   expects_explicit_underscore_id_index,
 *   requires_capped,
 *   requires_non_retryable_commands,
 *   # cloneCollectionAsCapped doesn't exist on mongos.
 *   assumes_against_mongod_not_mongos,
 *   # capped collections connot be sharded
 *   assumes_unsharded_collection,
 *   featureFlagRecordIdsReplicated,
 *   # cloneCollectionAsCapped (and capped collections) are not supported on serverless
 *   tenant_migration_incompatible,
 * ]
 */

const collName = jsTestName() + '_coll';
const cappedCollName = collName + '_capped';
const badCappedCollName = collName + '_bad_coll';
db[collName].drop();
db[cappedCollName].drop();
db[badCappedCollName].drop();

jsTestLog("Creating collection with recordIdsReplicated:true...");
assert.commandWorked(db.runCommand({create: collName, recordIdsReplicated: true}));
assert.commandWorked(db[collName].createIndex({a: 1}));

jsTestLog("Cloning as capped should work but disables recordIdsReplicated on clone.");
assert.commandWorked(
    db.runCommand({cloneCollectionAsCapped: collName, toCollection: cappedCollName, size: 2000}));
let collectionOptions = db[cappedCollName].exists();
assert(collectionOptions.options.capped, collectionOptions);
assert(!collectionOptions.options.recordIdsReplicated, collectionOptions);

jsTestLog("The new collection should not preserve secondary index.");
let indexes = db[cappedCollName].getIndexes();
assert.eq(indexes.length, 1, indexes);
jsTestLog("The original collection should still have secondary indexes.");
indexes = db[collName].getIndexes();
assert.eq(indexes.length, 2, indexes);

jsTestLog('Converting to capped should work but disables recordIdsReplicated after conversion.');
assert.commandWorked(db.runCommand({convertToCapped: collName, size: 2000}));
collectionOptions = db[collName].exists();
assert(collectionOptions.options.capped, collectionOptions);
assert(!collectionOptions.options.recordIdsReplicated, collectionOptions);

jsTestLog('Converting to capped should remove secondary indexes.');
indexes = db[collName].getIndexes();
assert.eq(indexes.length, 1, indexes);

jsTestLog(
    "Creating collection with capped:true and recordIdsReplicated:true should not be allowed.");
assert.commandFailedWithCode(
    db.runCommand({create: badCappedCollName, capped: true, size: 2000, recordIdsReplicated: true}),
    ErrorCodes.CommandNotSupported);
