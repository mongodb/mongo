/**
 * Basic test for the drop collection command
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: isbdgrid.
 *   not_allowed_with_signed_security_token,
 *   # Cannot implicitly shard accessed collections because of collection
 *   # existing when none expected.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   no_selinux,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let _collCounter = 0;
function getNewColl(db) {
    const collNamePrefix = jsTestName() + '_coll_';
    const coll = db[collNamePrefix + _collCounter++];
    return coll;
}

function assertCollectionDropped(coll) {
    // No more coll entry
    assert.eq(null, coll.exists(), "Collection exists after being dropped.");
    // No more documents
    assert.eq(0, coll.countDocuments({}), "Found leftover documents for dropped collection.");
    // No more indexes
    assert.eq(0, coll.getIndexes().length, "Found leftover indexes for dropped collection.");
}

function getExpectedNumIndexes(coll, initialNum) {
    // Sharded collections has one extra index on the shardKey.
    if (FixtureHelpers.isMongos(coll.getDB()) && FixtureHelpers.isSharded(coll)) {
        return initialNum + 1;
    }
    return initialNum;
}

jsTest.log("Drop Unexistent collection.");
{
    const coll = getNewColl(db);
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll);
}

jsTest.log("Drop existing collection.");
{
    const coll = getNewColl(db);
    // Create the collection
    assert.commandWorked(coll.insert({x: 1}));
    assert.eq(1, coll.countDocuments({x: 1}));
    assert.eq(getExpectedNumIndexes(coll, 1), coll.getIndexes().length);
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll);

    // Test idempotency
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll);
}

jsTest.log("Drop collection with multiple indexes.");
{
    const coll = getNewColl(db);
    assert.commandWorked(coll.insert({x: 1}));
    assert.eq(1, coll.countDocuments({x: 1}));
    coll.createIndex({a: 1});
    assert.eq(getExpectedNumIndexes(coll, 2), coll.getIndexes().length);
    assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index: "*"}));
    assert.eq(1, coll.getIndexes().length);
    // Drop the collection
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assertCollectionDropped(coll);
}
