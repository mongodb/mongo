/**
 * Tests that collStats does not return expensive creationString field when numericOnly is true.
 * @tags: [requires_wiredtiger, requires_fcv_52, requires_collstats]
 */
(function() {
"use strict";

// Grab the storage engine, default is wiredTiger
var storageEngine = jsTest.options().storageEngine || "wiredTiger";

// Although this test is tagged with 'requires_wiredtiger', this is not sufficient for ensuring
// that the parallel suite runs this test only on WT configurations. See SERVER-36181.
if (storageEngine !== 'wiredTiger') {
    jsTest.log('Skipping test because storageEngine is not "wiredTiger"');
    return;
}

const dbName = "test";
const collName = "collStats_numericOnly";

const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({a: 1}));

{
    // The collStats result should not contain the creationString field.
    const res = assert.commandWorked(testDB.runCommand({collStats: collName, numericOnly: true}));
    assert(res.hasOwnProperty("wiredTiger"));
    assert(res.wiredTiger.hasOwnProperty("cache"));
    assert(!res.wiredTiger.hasOwnProperty("creationString"));
    assert(!res.wiredTiger.hasOwnProperty("metadata"));
}

{
    // By default the creationString field should exist.
    const res = assert.commandWorked(testDB.runCommand({collStats: collName}));
    assert(res.hasOwnProperty("wiredTiger"));
    assert(res.wiredTiger.hasOwnProperty("cache"));
    assert(res.wiredTiger.hasOwnProperty("creationString"));
    assert(res.wiredTiger.hasOwnProperty("metadata"));
}
})();
