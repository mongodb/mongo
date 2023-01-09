/**
 * Ensures that creating an index with the same key but different name returns the
 * 'IndexOptionsConflict' error.
 */
(function() {
'use strict';

const coll = "create_index_same_spec_different_name";
db.coll.drop();

assert.commandWorked(db.runCommand({createIndexes: coll, indexes: [{key: {x: 1}, name: "x_1"}]}));

assert.commandFailedWithCode(
    db.runCommand({createIndexes: coll, indexes: [{key: {x: 1}, name: "x_2"}]}),
    ErrorCodes.IndexOptionsConflict);
}());
