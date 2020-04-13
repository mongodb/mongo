/**
 * Confirms slow currentOp logging does not conflict with applying an oplog batch.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary.
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({_id: 'a'}));

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
assert.commandWorked(secondaryDB.adminCommand({
    configureFailPoint: 'hangAfterCollectionInserts',
    mode: 'alwaysOn',
    data: {
        collectionNS: coll.getFullName(),
        first_id: 'b',
    },
}));

try {
    assert.commandWorked(coll.insert({_id: 'b'}));
    checkLog.containsJson(secondary, 20289);

    jsTestLog('Running currentOp() with slow operation logging.');
    // Lower slowms to make currentOp() log slow operation while the secondary is procesing the
    // commitIndexBuild oplog entry during oplog application.
    // Use admin db on secondary to avoid lock conflict with inserts in test db.
    const secondaryAdminDB = secondaryDB.getSiblingDB('admin');
    const profileResult = assert.commandWorked(secondaryAdminDB.setProfilingLevel(0, {slowms: -1}));
    jsTestLog('Configured profiling to always log slow ops: ' + tojson(profileResult));
    const currentOpResult = assert.commandWorked(secondaryAdminDB.currentOp());
    jsTestLog('currentOp() with slow operation logging: ' + tojson(currentOpResult));
    assert.commandWorked(
        secondaryAdminDB.setProfilingLevel(profileResult.was, {slowms: profileResult.slowms}));
    jsTestLog('Completed currentOp() with slow operation logging.');
} finally {
    assert.commandWorked(
        secondaryDB.adminCommand({configureFailPoint: 'hangAfterCollectionInserts', mode: 'off'}));
}

rst.stopSet();
})();
