/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * This test updates and deletes a document on the source between phases 1 and 2. The
 * secondary will initially fail to apply the update operation in phase 3 and subsequently have
 * to attempt to check the source for a new copy of the document. Before the secondary checks the
 * source, we insert a new copy of the document on the source so that the secondary can fetch it.
 */

(function() {
    var name = 'initial_sync_update_missing_doc2';
    var replSet = new ReplSetTest({
        name: name,
        nodes: [{}, {rsConfig: {arbiterOnly: true}}],
    });

    replSet.startSet();
    replSet.initiate();
    var primary = replSet.getPrimary();

    var coll = primary.getDB('test').getCollection(name);
    assert.writeOK(coll.insert({_id: 0, x: 1}));

    // Add a secondary node but make it hang after retrieving the last op on the source
    // but before copying databases.
    var secondary = replSet.add();
    secondary.setSlaveOk();

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeGettingMissingDocument', mode: 'alwaysOn'}));
    replSet.reInitiate();

    // Wait for fail point message to be logged.
    var checkLog = function(node, msg) {
        assert.soon(function() {
            var logMessages = assert.commandWorked(node.adminCommand({getLog: 'global'})).log;
            for (var i = 0; i < logMessages.length; i++) {
                if (logMessages[i].indexOf(msg) != -1) {
                    return true;
                }
            }
            return false;
        }, 'Did not see a log entry containing the following message: ' + msg, 60000, 1000);
    };
    checkLog(secondary, 'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    assert.writeOK(coll.update({_id: 0}, {x: 2}, {upsert: false, writeConcern: {w: 1}}));
    assert.writeOK(coll.remove({_id: 0}, {justOne: true, writeConcern: {w: 1}}));

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    checkLog(secondary, 'update of non-mod failed');
    checkLog(secondary, 'adding missing object');

    checkLog(secondary,
             'initial sync - initialSyncHangBeforeGettingMissingDocument fail point enabled');
    var doc = {_id: 0, x: 3};
    // Re-insert deleted document.
    assert.writeOK(coll.insert(doc, {writeConcern: {w: 1}}));

    secondary.getDB('test').setLogLevel(1, 'replication');
    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeGettingMissingDocument', mode: 'off'}));

    checkLog(secondary, 'inserted missing doc:');
    secondary.getDB('test').setLogLevel(0, 'replication');

    checkLog(secondary, 'initial sync done');

    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();

    var coll = secondary.getDB('test').getCollection(name);
    assert.eq(1, coll.find().itcount(), 'collection successfully synced to secondary');
    assert.eq(doc, coll.findOne(), 'document on secondary matches primary');

    replSet.stopSet();
})();
