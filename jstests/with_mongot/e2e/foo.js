// This test asserts that search e2e suites were correctly configured to spin up mongot(s) by
// checking that the mongotHost server parameter is set. Search indexes are created and a search
// query is ran to assert that no errors are thrown.

const coll = db.foo;
coll.drop();
coll.insert({a: -1, size: "small"});
coll.insert({a: -10, size: "medium", mood: "hungry"});
coll.insert({a: 100, size: "medium", mood: "very hungry"});

// A sanity check.
let result = coll.aggregate([{$match: {size: "medium"}}]).toArray();
assert.eq(result.length, 2);
// Confirm that mongod was launched with a connection string to mongot on localhost.
let paramOne = assert.commandWorked(db.adminCommand({getParameter: 1, "mongotHost": -1}));
assert(paramOne["mongotHost"].startsWith("localhost:"));
let paramTwo = assert.commandWorked(
    db.adminCommand({getParameter: 1, "searchIndexManagementHostAndPort": -1}));
assert.eq(paramOne["mongotHost"], paramTwo["searchIndexManagementHostAndPort"]);

// If a name is not specified during search index creation, mongot will name it default.
coll.createSearchIndex({name: "foo-block", definition: {"mappings": {"dynamic": true}}})
// createSearchIndex shell command default behavior is to block returning until mongot lists the new
// index as queryable eg blockUntilSearchIndexQueryable is true by default.
coll.createSearchIndex({name: "foo-non-block", definition: {"mappings": {"dynamic": true}}},
                       {blockUntilSearchIndexQueryable: false})
var searchIndexes = coll.aggregate([{"$listSearchIndexes": {}}]).toArray();
assert.eq(searchIndexes.length, 2, searchIndexes);

let searchRes = coll.aggregate([{
                        $search: {
                            index: "foo-block",
                            exists: {
                                path: "mood",
                            },
                        }
                    }])
                    .toArray();
assert.eq(searchRes.length, 2, searchRes);