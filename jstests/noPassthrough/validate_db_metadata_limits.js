/**
 * Tests to verify that the validateDBMetadata command returns response correctly when the expected
 * output data is larger than the max BSON size.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
const testDB = conn.getDB("validate_db_metadaba");
const coll = testDB.getCollection("test");

for (let i = 0; i < 100; i++) {
    // Create a large index name. As the index name is returned in the output validateDBMetadata
    // command, it can cause the output size to exceed max BSON size.
    let largeName = "a".repeat(200000);
    assert.commandWorked(testDB.runCommand(
        {createIndexes: "test" + i, indexes: [{key: {p: 1}, name: largeName, sparse: true}]}));
}

const res = assert.commandWorked(
    testDB.runCommand({validateDBMetadata: 1, apiParameters: {version: "1", strict: true}}));

assert(res.hasMoreErrors, res);
assert(res.apiVersionErrors, res);
assert(res.apiVersionErrors.length < 100, res);

MongoRunner.stopMongod(conn);
})();
