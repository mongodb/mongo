/**
 * A file used to open a backup cursor and copy data files from the current primary node.
 */

import {backupData} from "jstests/libs/backup_utils.js";

function takeBackup(conn) {
    jsTestLog("Magic Restore: Taking backup");
    // Take the initial checkpoint.
    assert.commandWorked(db.adminCommand({fsync: 1}));

    var dbpathPrefix = MongoRunner.dataPath + '../magicRestore';

    // Creates the directory if it doesn't exist already.
    mkdir(dbpathPrefix);
    let metadata = backupData(conn, dbpathPrefix);
    let testDB = conn.getDB("magic_restore_metadata");
    testDB.runCommand({
        insert: "magic_restore_checkpointTimestamp",
        documents: [{ts: metadata.checkpointTimestamp}]
    });
    jsTestLog("Magic Restore: Backup written to " + dbpathPrefix);
}

const conn = db.getMongo();
takeBackup(conn);
