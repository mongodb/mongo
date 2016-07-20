/**
 * This test tests that initial sync succeeds when the sync source's oplog rolls over before the
 * destination node reaches the oplog apply phase. It adds a new secondary to a replicaset and then
 * pauses the initial sync before it copies the databases but after the oplog buffer has begun
 * buffering oplog entries. The primary then fills up its oplog until it rolls over. At that point
 * initial sync is resumed and we assert that it succeeds and that all of the inserted documents
 * are there.
 */

(function() {
    "use strict";
    // If the parameter is already set, don't run this test.
    var parameters = db.adminCommand({getCmdLineOpts: 1}).parsed.setParameter;
    if (parameters.use3dot2InitialSync || parameters.initialSyncOplogBuffer) {
        jsTest.log("Skipping initial_sync_parameters.js because use3dot2InitialSync or " +
                   "initialSyncOplogBuffer was already provided.");
        return;
    }

    var name = 'initial_sync_oplog_rollover';
    var replSet = new ReplSetTest({
        name: name,
        nodes: 1,
    });

    var oplogSizeOnPrimary = 1;  // size in MB
    replSet.startSet({oplogSize: oplogSizeOnPrimary});
    replSet.initiate();
    var primary = replSet.getPrimary();

    var coll = primary.getDB('test').foo;
    assert.writeOK(coll.insert({a: 1}));
    var firstOplogEntry = primary.getDB('local').oplog.rs.find().sort({ts: 1}).limit(1)[0];

    // Add a secondary node but make it hang before copying databases.
    var secondary = replSet.add(
        {setParameter: {use3dot2InitialSync: false, initialSyncOplogBuffer: "collection"}});
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
        }, 'Did not see a log entry containing the following message: ' + msg, 10000, 1000);
    };
    checkLog(secondary, 'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    // Make documents around 100KB so that they overflow the oplog.
    const largeStr = new Array(16 * 1024 * oplogSizeOnPrimary).join('aaaaaaaa');
    for (var i = 0; i < 40; i++) {
        assert.writeOK(coll.insert({a: 2, x: i, long_str: largeStr}));
    }

    // Assert that the oplog has rolled over on the primary.
    assert.neq(primary.getDB('local').oplog.rs.find().sort({ts: 1}).limit(1)[0], firstOplogEntry);

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    replSet.awaitSecondaryNodes(200 * 1000);

    assert.eq(40,
              secondary.getDB('test').foo.count({a: 2}),
              'collection successfully synced to secondary');
})();
