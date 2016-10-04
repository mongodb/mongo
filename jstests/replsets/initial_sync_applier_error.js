/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source and fetches operations from sync source; and
 * 3) applies operations from the source after op_start1.
 *
 * This test creates, deletes and creates again an index on the source between phases 1 and 2.
 * The index that we create again is not exactly the same as the first index we created, but
 * it will have the same name. The secondary will initially fail to apply the operations in phase 3
 * and subsequently have to retry the initial sync.
 */

(function() {
    "use strict";
    var parameters = TestData.setParameters;
    if (parameters && parameters.indexOf("use3dot2InitialSync=true") != -1) {
        jsTest.log("Skipping this test because use3dot2InitialSync was provided.");
        return;
    }

    var name = 'initial_sync_applier_error';
    var replSet = new ReplSetTest({
        name: name,
        nodes: [{}, {rsConfig: {arbiterOnly: true}}],
    });

    replSet.startSet();
    replSet.initiate();
    var primary = replSet.getPrimary();

    var coll = primary.getDB('test').getCollection(name);
    assert.writeOK(coll.insert({_id: 0, content: "hi"}));

    // Add a secondary node but make it hang after retrieving the last op on the source
    // but before copying databases.
    var secondary = replSet.add({setParameter: "numInitialSyncAttempts=2"});
    secondary.setSlaveOk();

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
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

    assert.commandWorked(coll.createIndex({content: "text"}, {default_language: "spanish"}));
    assert.commandWorked(coll.dropIndex("content_text"));
    assert.commandWorked(coll.createIndex({content: "text"}, {default_language: "english"}));

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    checkLog(secondary, 'content_text already exists with different options');
    checkLog(secondary, 'initial sync done');

    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();

    var textIndexes =
        secondary.getDB('test').getCollection(name).getIndexes().filter(function(index) {
            return index.name == "content_text";
        });
    assert.eq(1, textIndexes.length);
    assert.eq("english", textIndexes[0].default_language);
})();
