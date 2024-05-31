// This test ensures correct error handling for the createSearchIndex shell command
const coll = db[jsTestName()];
coll.drop();

let error = assert.throws(
    () => coll.createSearchIndex({name: "foo-block", definition: {"mappings": {"dynamic": true}}},
                                 {blockUntilSearchIndexQueryable: "true"}));
let expectedMessage = "Error: 'blockUntilSearchIndexQueryable' argument must be a boolean";
assert.eq(error, expectedMessage);

error =
    assert.throws(() => coll.createSearchIndex(
                      {name: "foo-block", definition: {"mappings": {"dynamic": true}}}, {arg2: 1}));
expectedMessage =
    "Error: createSearchIndex only accepts index definition object and blockUntilSearchIndexQueryable object";
assert.eq(error, expectedMessage);

error = assert.throws(
    () => coll.createSearchIndex({name: "foo-block", definition: {"mappings": {"dynamic": true}}},
                                 {arg2: 1},
                                 {arg3: "three"}));
expectedMessage = "Error: createSearchIndex accepts up to 2 arguments";
assert.eq(error, expectedMessage);

error = assert.throws(() => coll.createSearchIndex({name: "foo-block"}));
expectedMessage = "Error: createSearchIndex must have a definition";
assert.eq(error, expectedMessage);