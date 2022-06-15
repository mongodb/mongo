// Tests that the update lookup of an unsharded change stream will use the collection-default
// collation, regardless of the collation on the change stream.
//
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("test");
const coll = db[jsTestName()];
const caseInsensitive = {
    locale: "en_US",
    strength: 2
};
assert.commandWorked(db.createCollection(coll.getName(), {collation: caseInsensitive}));

// Insert some documents that have similar _ids, but differ by case and diacritics. These _ids
// would all match the collation on the strengthOneChangeStream, but should not be confused
// during the update lookup using the strength 2 collection default collation.
assert.commandWorked(coll.insert({_id: "abc", x: "abc"}));
assert.commandWorked(coll.insert({_id: "abç", x: "ABC"}));
assert.commandWorked(coll.insert({_id: "åbC", x: "AbÇ"}));

const changeStreamDefaultCollation = coll.aggregate(
    [{$changeStream: {fullDocument: "updateLookup"}}, {$match: {"fullDocument.x": "abc"}}],
    {collation: caseInsensitive});

// Strength one will consider "ç" equal to "c" and "C".
const strengthOneCollation = {
    locale: "en_US",
    strength: 1
};
const strengthOneChangeStream = coll.aggregate(
    [{$changeStream: {fullDocument: "updateLookup"}}, {$match: {"fullDocument.x": "abc"}}],
    {collation: strengthOneCollation});

assert.commandWorked(coll.update({_id: "abc"}, {$set: {updated: true}}));

// Track the number of _id index usages to prove that the update lookup uses the _id index (and
// therefore is using the correct collation for the lookup).
function numIdIndexUsages() {
    return coll.aggregate([{$indexStats: {}}, {$match: {name: "_id_"}}]).toArray()[0].accesses.ops;
}
const idIndexUsagesBeforeIteration = numIdIndexUsages();

// Both cursors should produce a document describing this update, since the "x" value of the
// first document will match both filters.
assert.soon(() => changeStreamDefaultCollation.hasNext());
assert.docEq(changeStreamDefaultCollation.next().fullDocument,
             {_id: "abc", x: "abc", updated: true});
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 1);
assert.docEq(strengthOneChangeStream.next().fullDocument, {_id: "abc", x: "abc", updated: true});
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 2);

assert.commandWorked(coll.update({_id: "abç"}, {$set: {updated: true}}));
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 3);

// Again, both cursors should produce a document describing this update.
assert.soon(() => changeStreamDefaultCollation.hasNext());
assert.docEq(changeStreamDefaultCollation.next().fullDocument,
             {_id: "abç", x: "ABC", updated: true});
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 4);
assert.docEq(strengthOneChangeStream.next().fullDocument, {_id: "abç", x: "ABC", updated: true});
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 5);

assert.commandWorked(coll.update({_id: "åbC"}, {$set: {updated: true}}));
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 6);

// Both $changeStream stages will see this update and both will look up the full document using
// the foreign collection's default collation. However, the changeStreamDefaultCollation's
// subsequent $match stage will reject the document because it does not consider "AbÇ" equal to
// "abc". Only the strengthOneChangeStream will output the final document.
assert.soon(() => strengthOneChangeStream.hasNext());
assert.docEq(strengthOneChangeStream.next().fullDocument, {_id: "åbC", x: "AbÇ", updated: true});
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 7);
assert(!changeStreamDefaultCollation.hasNext());
assert.eq(numIdIndexUsages(), idIndexUsagesBeforeIteration + 8);

changeStreamDefaultCollation.close();
strengthOneChangeStream.close();
rst.stopSet();
}());
