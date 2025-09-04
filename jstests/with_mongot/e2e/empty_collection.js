// This test confirms behavior when a search query is executed on an empty and non-existent collection.
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

let collName = jsTestName();
let coll = db[collName];
db.createCollection(collName);
coll.insertOne({_id: 1, a: "bar"});

// Create search index.
const indexName = collName + "_index";
createSearchIndex(coll, {name: indexName, definition: {mappings: {dynamic: true}}});

// Delete the document (collection still exists) and check that a search query correctly returns no results.
coll.deleteOne({_id: 1});
let result = coll.aggregate([{$search: {index: indexName, text: {path: "a", query: "bar"}}}]).toArray();
assert.eq(0, result.length, result);

// Drop the collection and check that a search query correctly returns no results.
coll.drop();
result = coll.aggregate([{$search: {index: indexName, text: {path: "a", query: "bar"}}}]).toArray();
assert.eq(0, result.length, result);
