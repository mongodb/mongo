// Test for SERVER-7017
// We are allowed to rename a collection when one of its indexes will generate a namespace
// that is greater than 120 chars. To do this we create a long index name and try
// and rename the collection to one with a much longer name. We use the test database
// by default and we add this here to ensure we are using it
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
//   requires_non_retryable_commands,
// ]

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB("test");
const srcName = "renameSRC";
const src = testDB.getCollection(srcName);

const collLength = 250;
const longDstName = 'a'.repeat(collLength);
const dst = testDB.getCollection(longDstName);

src.drop();
dst.drop();

src.createIndex({
    "name": 1,
    "date": 1,
    "time": 1,
    "renameCollection": 1,
    "mongodb": 1,
    "testing": 1,
    "data": 1
});

// Newly created index + _id index in original collection (+ hashed _id in case of sharded suite)
const originalNumberOfIndexes = src.getIndexes().length;

// Should succeed in renaming collection as the long index namespace is acceptable.
assert.commandWorked(src.renameCollection(longDstName), "Rename collection with long name failed");

assert.eq(0, src.getIndexes().length, "No indexes expected on already renamed collection");
assert.eq(originalNumberOfIndexes,
          dst.getIndexes().length,
          "Expected exactly same number of indexes of source collection before rename");

// Renaming a collection over 255 characters fails.
const longDstNameInvalid = 'a'.repeat(255);
db.createCollection(srcName);
assert.commandFailedWithCode(src.renameCollection(longDstNameInvalid),
                             [ErrorCodes.InvalidNamespace, ErrorCodes.IllegalOperation]);
