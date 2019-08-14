/**
 * Tests that initial sync is completed successfully if a 'renameCollection' operation
 * occurs on the sync source during initial sync.
 * See SERVER-4941.
 */

(function() {
'use strict';

load('jstests/replsets/rslib.js');
const basename = 'initial_sync_rename_collection';

jsTestLog('Bring up a replica set');
const rst = new ReplSetTest({name: basename, nodes: 1});
rst.startSet();
rst.initiate();

const db0_name = "db0";
const db1_name = "db1";

const primary = rst.getPrimary();

// Create two separate databases so that we can rename a collection across databases.
const primary_db0 = primary.getDB(db0_name);
const primary_db1 = primary.getDB(db1_name);

jsTestLog("Create collections on primary");
const collRenameWithinDB_name = 'coll_1';
const collRenameAcrossDBs_name = 'coll_2';
const collWithinFinal_name = 'renamed';
const collAcrossFinal_name = 'renamed_across';

// Create two collections on the same database. One will be renamed within the database
// and the other will be renamed to a different database.
assert.commandWorked(primary_db0[collRenameWithinDB_name].save({}));
assert.commandWorked(primary_db0[collRenameAcrossDBs_name].save({}));

jsTestLog('Waiting for replication');
rst.awaitReplication();

jsTestLog('Bring up a new node');
const secondary = rst.add({setParameter: 'numInitialSyncAttempts=1'});

// Add a fail point that causes the secondary's initial sync to hang before
// copying databases.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'alwaysOn'}));

jsTestLog('Begin initial sync on secondary');
let conf = rst.getPrimary().getDB('admin').runCommand({replSetGetConfig: 1}).config;
conf.members.push({_id: 1, host: secondary.host, priority: 0, votes: 0});
conf.version++;
assert.commandWorked(rst.getPrimary().getDB('admin').runCommand({replSetReconfig: conf}));
assert.eq(primary, rst.getPrimary(), 'Primary changed after reconfig');

// Confirm that initial sync started on the secondary node.
jsTestLog('Waiting for initial sync to start');
checkLog.contains(secondary,
                  'initial sync - initialSyncHangBeforeCopyingDatabases fail point enabled');

// Start renaming collections while initial sync is hanging.
jsTestLog('Rename collection ' + db0_name + '.' + collRenameWithinDB_name + ' to ' + db0_name +
          '.' + collWithinFinal_name + ' on the sync source ' + db0_name);
assert.commandWorked(primary_db0[collRenameWithinDB_name].renameCollection(collWithinFinal_name));

jsTestLog('Rename collection ' + db0_name + '.' + collRenameAcrossDBs_name + ' to ' + db1_name +
          '.' + collAcrossFinal_name + ' on the sync source ' + db0_name);
assert.commandWorked(primary.adminCommand({
    renameCollection: primary_db0[collRenameAcrossDBs_name].getFullName(),
    to: primary_db1[collAcrossFinal_name]
            .getFullName()  // Collection 'renamed_across' is implicitly created.
}));

// Disable fail point so that the secondary can finish its initial sync.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

jsTestLog('Wait for both nodes to be up-to-date');
rst.awaitSecondaryNodes();
rst.awaitReplication();

const secondary_db0 = secondary.getDB(db0_name);
const secondary_db1 = secondary.getDB(db1_name);

jsTestLog('Check that collection was renamed correctly on the secondary');
assert.eq(
    secondary_db0[collWithinFinal_name].find().itcount(), 1, 'renamed collection does not exist');
assert.eq(secondary_db1[collAcrossFinal_name].find().itcount(),
          1,
          'renamed_across collection does not exist');
assert.eq(
    secondary_db0[collRenameWithinDB_name].find().itcount(),
    0,
    'collection ' + collRenameWithinDB_name + ' still exists after it was supposed to be renamed');
assert.eq(
    secondary_db0[collRenameAcrossDBs_name].find().itcount(),
    0,
    'collection ' + collRenameAcrossDBs_name + ' still exists after it was supposed to be renamed');

rst.checkReplicatedDataHashes();
rst.checkOplogs();
rst.stopSet();
})();
