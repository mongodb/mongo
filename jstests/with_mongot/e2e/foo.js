// This test asserts that search e2e suites were correctly configured to spin up mongot(s) by
// checking that the mongotHost server parameter is set. A search index is created and a search
// query is ran to assert that no errors are thrown.

const coll = db.foo;
coll.drop()
coll.insert({a: -1, size: "small"})
coll.insert({a: -10, size: "medium", mood: "hungry"})
coll.insert({a: 100, size: "medium", mood: "very hungry"})

// A sanity check.
let result = coll.aggregate([{$match: {size: "medium"}}]).toArray();
assert.eq(result.length, 2);
// Confirm that mongod was launched with a connection string to mongot on localhost.
let paramOne = assert.commandWorked(db.adminCommand({getParameter: 1, "mongotHost": -1}));
assert(paramOne["mongotHost"].startsWith("localhost:"));
let paramTwo = assert.commandWorked(
    db.adminCommand({getParameter: 1, "searchIndexManagementHostAndPort": -1}));
assert.eq(paramOne["mongotHost"], paramTwo["searchIndexManagementHostAndPort"]);
// TODO SERVER-86614 replace this with shell helper that waits for mongot index definitions to be
// stable.
let searchIndexResult = assert.commandWorked(db.runCommand(
    {'createSearchIndexes': "foo", 'indexes': [{'definition': {'mappings': {'dynamic': true}}}]}));
assert.doesNotThrow(() => coll.aggregate([{"$listSearchIndexes": {}}]))
// TODO SERVER-86616 replace this $search query with shell helper and assert that results are
// correct.
let searchRes = assert.doesNotThrow(() => coll.aggregate([{
    $search: {
        exists: {
            path: "mood",
        }
    }
}]));
