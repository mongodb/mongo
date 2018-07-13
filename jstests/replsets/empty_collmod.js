/*
 * Tests that an empty coll mod request succeeds and is not added to the oplog
 */

(function() {
    "use strict";
    let replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();
    let primary = replTest.getPrimary();
    let primaryDB = primary.getDB('emptyCollModDB');
    assert.commandWorked(primaryDB.createCollection('foo'));
    let coll = primaryDB.getCollection('foo');
    let oplogNumEntries = primary.getDB('local').oplog.rs.count();
    let oplogData = primary.getDB('local').oplog.rs.find().toArray();
    assert.commandWorked(primaryDB.runCommand({collMod: 'foo'}));
    assert.eq(primary.getDB('local').oplog.rs.find().toArray(), oplogData);
    assert.eq(primary.getDB('local').oplog.rs.count(), oplogNumEntries);
    replTest.stopSet();
}());
