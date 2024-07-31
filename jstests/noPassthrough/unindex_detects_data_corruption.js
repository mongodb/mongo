/**
 * This tests that errors are logged when unindexing _id finds evidence of corruption, the server
 * does not crash, and the appropriate error is returned.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();

const db = primary.getDB('test');
const collName = 'coll';
const coll = db[collName];

assert.commandWorked(coll.insert({a: "first"}));

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "WTIndexUassertDuplicateRecordForKeyOnIdUnindex", mode: "alwaysOn"}));

assert.commandFailedWithCode(coll.remove({a: "first"}), ErrorCodes.DataCorruptionDetected);

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "WTIndexUassertDuplicateRecordForKeyOnIdUnindex", mode: "off"}));

assert.soonNoExcept(() => {
    // The health log entry is written asynchronously by a background worker, expect it to be
    // eventually found.
    let entry = primary.getDB('local').system.healthlog.findOne({severity: 'error'});
    assert(entry, "No healthlog entry found on " + tojson(primary));
    assert.eq("Un-index seeing multiple records for key", entry.msg, tojson(entry));
    assert.eq(1, primary.getDB('local').system.healthlog.count({severity: 'error'}));
    return true;
});

replSet.stopSet();
})();
