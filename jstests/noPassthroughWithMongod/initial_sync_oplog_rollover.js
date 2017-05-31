/**
 * This test tests that initial sync succeeds when the sync source's oplog rolls over before the
 * destination node reaches the oplog apply phase. It adds a new secondary to a replicaset and then
 * pauses the initial sync before it copies the databases but after it starts to fetch and buffer
 * oplog entries. The primary then fills up its oplog until it rolls over. At that point
 * initial sync is resumed and we assert that it succeeds and that all of the inserted documents
 * are there.
 */

(function() {
    "use strict";
    load("jstests/libs/check_log.js");

    // If the parameter is already set, don't run this test.
    var parameters = db.adminCommand({getCmdLineOpts: 1}).parsed.setParameter;
    if (parameters.use3dot2InitialSync || parameters.initialSyncOplogBuffer) {
        jsTest.log("Skipping initial_sync_oplog_rollover.js because use3dot2InitialSync or " +
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

    function getFirstOplogEntry(conn) {
        return conn.getDB('local').oplog.rs.find().sort({$natural: 1}).limit(1)[0];
    }

    var firstOplogEntry = getFirstOplogEntry(primary);

    // Add a secondary node but make it hang before copying databases.
    var secondary = replSet.add(
        {setParameter: {use3dot2InitialSync: false, initialSyncOplogBuffer: "collection"}});
    secondary.setSlaveOk();

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));
    replSet.reInitiate();

    checkLog.contains(secondary,
                      'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

    // Keep inserting large documents until they roll over the oplog.
    const largeStr = new Array(4 * 1024 * oplogSizeOnPrimary).join('aaaaaaaa');
    var i = 0;
    while (bsonWoCompare(getFirstOplogEntry(primary), firstOplogEntry) === 0) {
        assert.writeOK(coll.insert({a: 2, x: i++, long_str: largeStr}));
        sleep(100);
    }

    assert.commandWorked(secondary.getDB('admin').runCommand(
        {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

    replSet.awaitSecondaryNodes(200 * 1000);

    assert.eq(i,
              secondary.getDB('test').foo.count({a: 2}),
              'collection successfully synced to secondary');

    assert.eq(0,
              secondary.getDB('local')['temp_oplog_buffer'].find().itcount(),
              "Oplog buffer was not dropped after initial sync");
})();
