/**
 * Projection tests for FTS queries.
 * @tags: [
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

const kUnavailableMetadataErrCode = 40218;

const coll = db.text_proj;
coll.drop();

assert.commandWorked(coll.insert({_id: 1, x: "a", y: "b", z: "c"}));
assert.commandWorked(coll.insert({_id: 2, x: "d", y: "e", z: "f"}));
assert.commandWorked(coll.insert({_id: 3, x: "a", y: "g", z: "h"}));

assert.commandWorked(coll.createIndex({x: "text"}, {default_language: "none"}));

let res = coll.find({"$text": {"$search": "a"}}).toArray();
assert.eq(2, res.length, res);
assert(res[0].y, res);

res = coll.find({"$text": {"$search": "a"}}, {x: 1}).toArray();
assert.eq(2, res.length, res);
assert(!res[0].y, res);

// Text score $meta projection fails if there is no $text predicate, for both find and agg.
let error = assert.throws(() => coll.find({}, {score: {$meta: "textScore"}}).itcount());
assert.commandFailedWithCode(error, kUnavailableMetadataErrCode);
error = assert.throws(() => coll.aggregate([{$project: {score: {$meta: "textScore"}}}]).itcount());
assert.commandFailedWithCode(error, kUnavailableMetadataErrCode);
}());
