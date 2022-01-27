/**
 * Tests that the createIndex command accepts a comment field with BSONObj and this comment is
 * retrievable with listIndexes called by getIndexes.
 *
 * @tags: [cannot_create_unique_index_when_using_hashed_shard_key, requires_fcv_53]
 */
(function() {
"use strict";

const t = db.index_comment;
t.drop();

const collModIndexUniqueEnabled = assert
                                      .commandWorked(db.getMongo().adminCommand(
                                          {getParameter: 1, featureFlagCollModIndexUnique: 1}))
                                      .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled.');
    return;
}

// Attempts to build an index with invalid comments fails.
['not an object', 1.0, [0, 1, 2], null, true].forEach(invalidObj => {
    assert.commandFailedWithCode(t.createIndex({a: 1}, {comment: invalidObj}),
                                 ErrorCodes.TypeMismatch);
});

const validComment = {
    "key": "value"
};

// Check that adding a comment field on a regular index works.
assert.commandWorked(t.createIndex({a: 1}, {comment: validComment}));

// Check that we are able to insert a document into that index.
assert.commandWorked(t.insert({a: "thing"}));

// Check that trying to create the same index with a different comment fails.
assert.commandFailedWithCode(t.createIndex({a: 1}, {comment: {"a different comment": 1}}),
                             ErrorCodes.IndexOptionsConflict);

// Check that adding a comment field on a unique index works.
assert.commandWorked(t.createIndex({b: 1}, {unique: true, comment: validComment}));

// Check that the comment field exists on both indexes when getIndexes is called.
const indexesWithComments = t.getIndexes().filter(function(doc) {
    return friendlyEqual(doc.comment, validComment);
});
assert.eq(2, indexesWithComments.length);
})();
