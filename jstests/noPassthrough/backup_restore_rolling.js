/**
 * Test the backup/restore process:
 * - 3 node replica set
 * - Mongo CRUD client
 * - Mongo FSM client
 * - fsyncLock (or stop) Secondary
 * - cp (or rsync) DB files
 * - fsyncUnlock (or start) Secondary
 * - Start mongod as hidden secondary
 * - Wait until new hidden node becomes secondary
 *
 * Some methods for backup used in this test checkpoint the files in the dbpath. This technique will
 * not work for ephemeral storage engines, as they do not store any data in the dbpath.
 * @tags: [
 *   requires_persistence,
 * ]
 */

load("jstests/noPassthrough/libs/backup_restore.js");

(function() {
"use strict";

// Windows doesn't guarantee synchronous file operations.
if (_isWindows()) {
    print("Skipping test on windows");
    return;
}

// Grab the storage engine, default is wiredTiger
var storageEngine = jsTest.options().storageEngine || "wiredTiger";

// if rsync is not available on the host, then this test is skipped
if (!runProgram('bash', '-c', 'which rsync')) {
    new BackupRestoreTest({backup: 'rolling', clientTime: 30000}).run();
} else {
    jsTestLog("Skipping test for " + storageEngine + ' rolling');
}
}());
