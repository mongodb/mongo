/**
 * This test validates that dropSearchIndex works for standalone and sharded configurations.
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

const docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({_id: i, n: 9 - i});
}
coll.insertMany(docs);

let error = assert.throws(() => dropSearchIndex(coll, {id: 1029384712}));
let expectedMessage = "Error: dropSearchIndex library helper only accepts a search index name";
assert.eq(error, expectedMessage);

let indexDef = {mappings: {dynamic: true}};
createSearchIndex(coll, {name: "searchIndexToDrop", definition: indexDef});
dropSearchIndex(coll, {name: "searchIndexToDrop"});
