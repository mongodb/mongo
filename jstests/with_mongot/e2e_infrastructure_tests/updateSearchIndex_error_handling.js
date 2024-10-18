/**
 * This test ensures correct error handling for the updateSearchIndex shell command.
 * @tags: [
 * requires_mongot_1_42
 * ]
 */

import {updateSearchIndex} from "jstests/libs/search.js";

const coll = db[jsTestName()];
coll.drop();

let error = assert.throws(
    () => updateSearchIndex(coll,
                            {name: "foo-block", definition: {"mappings": {"dynamic": true}}},
                            {blockUntilSearchIndexQueryable: "true"}));
let expectedMessage = "Error: 'blockUntilSearchIndexQueryable' argument must be a boolean";
assert.eq(error, expectedMessage);

error = assert.throws(
    () => updateSearchIndex(
        coll, {name: "foo-block", definition: {"mappings": {"dynamic": true}}}, {arg2: 1}));
expectedMessage =
    "Error: updateSearchIndex only accepts index definition object and blockUntilSearchIndexQueryable object";
assert.eq(error, expectedMessage);

error = assert.throws(() => updateSearchIndex(coll, {name: "foo-block"}));
expectedMessage = "Error: updateSearchIndex must have a definition";
assert.eq(error, expectedMessage);