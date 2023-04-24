/**
 * Basic test for the drop collection command
 * The test runs commands that are not allowed with security token: isbdgrid.
 * @tags: [
 *   not_allowed_with_security_token,
 *   # Cannot implicitly shard accessed collections because of collection
 *   # existing when none expected.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   no_selinux,
 * ]
 */

(function() {
"use strict";

function assertCollectionDropped(ns) {
    const coll = db[ns];
    // No more coll entry
    assert.eq(null, coll.exists(), "Collection exists after being dropped.");
    // No more documents
    assert.eq(0, coll.countDocuments({}), "Found leftover documents for dropped collection.");
    // No more indexes
    assert.eq(0, coll.getIndexes().length, "Found leftover indexes for dropped collection.");
}

const coll = db[jsTestName() + "_coll"];

jsTest.log("Drop Unexistent collection.");
{
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());
}

jsTest.log("Drop existing collection.");
{
    // Create the collection
    assert.commandWorked(coll.insert({x: 1}));
    assert.eq(1, coll.countDocuments({x: 1}));
    assert.eq(1, coll.getIndexes().length);
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());

    // Test idempotency
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());
}

jsTest.log("Drop collection with multiple indexes.");
{
    assert.commandWorked(coll.insert({x: 1}));
    assert.eq(1, coll.countDocuments({x: 1}));
    coll.createIndex({a: 1});
    assert.eq(2, coll.getIndexes().length);
    assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index: "*"}));
    assert.eq(1, coll.getIndexes().length);
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll.getFullName());
}
})();
