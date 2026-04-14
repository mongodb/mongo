/**
 * Tests that capped collections can be created with recordIdsReplicated:true. Also tests
 * that running 'cloneCollectionAsCapped' and 'convertToCapped' maintain the recordIdsReplicated:true property.
 *
 * @tags: [
 *   expects_explicit_underscore_id_index,
 *   requires_capped,
 *   requires_non_retryable_commands,
 *   # cloneCollectionAsCapped doesn't exist on mongos.
 *   assumes_against_mongod_not_mongos,
 *   featureFlagRecordIdsReplicated,
 * ]
 */

import {hasRecordIdsReplicated} from "jstests/libs/collection_write_path/replicated_record_ids_utils.js";

const collName = jsTestName() + "_coll";
const cappedCollName = collName + "_capped";
const createdAsCappedCollName = collName + "_created_as_capped_coll";
db[collName].drop();
db[cappedCollName].drop();
db[createdAsCappedCollName].drop();

assert.commandWorked(db.runCommand({create: collName}));
assert.commandWorked(db[collName].createIndex({a: 1}));

jsTestLog("Cloning as capped should work with replicatedRecordIds and preserve the option.");
assert.commandWorked(db.runCommand({cloneCollectionAsCapped: collName, toCollection: cappedCollName, size: 2000}));
let collectionOptions = db[cappedCollName].exists();
assert(collectionOptions.options.capped, collectionOptions);
assert(
    hasRecordIdsReplicated(db, cappedCollName),
    "capped collection should have recordIdsReplicated set after cloneCollectionAsCapped",
);
let indexes = db[cappedCollName].getIndexes();
assert.eq(indexes.length, 1, indexes);
indexes = db[collName].getIndexes();
assert.eq(indexes.length, 2, indexes);

jsTestLog("Converting to capped should work with recordIdsReplicated.");
assert.commandWorked(db.runCommand({convertToCapped: collName, size: 2000}));
collectionOptions = db[collName].exists();
assert(collectionOptions.options.capped, collectionOptions);
assert(hasRecordIdsReplicated(db, collName), "collection should have recordIdsReplicated set after convertToCapped");
indexes = db[collName].getIndexes();
assert.eq(indexes.length, 1, indexes);

jsTestLog("Creating collection with capped:true and recordIdsReplicated:true should be allowed.");
assert.commandWorked(db.runCommand({create: createdAsCappedCollName, capped: true, size: 2000}));
collectionOptions = db[createdAsCappedCollName].exists();
assert(collectionOptions.options.capped, collectionOptions);
assert(
    hasRecordIdsReplicated(db, createdAsCappedCollName),
    "capped collection should have recordIdsReplicated set after creation",
);
