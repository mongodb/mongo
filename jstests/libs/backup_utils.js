load("jstests/libs/parallelTester.js");  // for Thread.

function backupData(mongo, destinationDirectory) {
    let backupCursor = openBackupCursor(mongo);
    let metadata = getBackupCursorMetadata(backupCursor);
    copyBackupCursorFiles(backupCursor, metadata.dbpath, destinationDirectory);
    backupCursor.close();
    return metadata;
}

function openBackupCursor(mongo) {
    // Opening a backup cursor can race with taking a checkpoint, resulting in a transient
    // error. Retry until it succeeds.
    while (true) {
        try {
            return mongo.getDB("admin").aggregate([{$backupCursor: {}}]);
        } catch (exc) {
            jsTestLog({"Failed to open a backup cursor, retrying.": exc});
        }
    }
}

function extendBackupCursor(mongo, backupId, extendTo) {
    return mongo.getDB("admin").aggregate(
        [{$backupCursorExtend: {backupId: backupId, timestamp: extendTo}}],
        {maxTimeMS: 180 * 1000});
}

function startHeartbeatThread(host, backupCursor, session, stopCounter) {
    let cursorId = tojson(backupCursor._cursorid);
    let lsid = tojson(session.getSessionId());

    let heartbeatBackupCursor = function(host, cursorId, lsid, stopCounter) {
        const conn = new Mongo(host);
        const db = conn.getDB("admin");
        while (stopCounter.getCount() > 0) {
            let res = assert.commandWorked(db.runCommand({
                getMore: eval("(" + cursorId + ")"),
                collection: "$cmd.aggregate",
                lsid: eval("(" + lsid + ")")
            }));
            sleep(10 * 1000);
        }
    };

    heartbeater = new Thread(heartbeatBackupCursor, host, cursorId, lsid, stopCounter);
    heartbeater.start();
    return heartbeater;
}

function getBackupCursorMetadata(backupCursor) {
    assert(backupCursor.hasNext());
    let doc = backupCursor.next();
    assert(doc.hasOwnProperty("metadata"));
    return doc["metadata"];
}

/**
 * Exhaust the backup cursor and copy all the listed files to the destination directory. If `async`
 * is true, this function will spawn a Thread doing the copy work and return the thread along
 * with the backup cursor metadata. The caller should `join` the thread when appropriate.
 */
function copyBackupCursorFiles(backupCursor, dbpath, destinationDirectory, async) {
    resetDbpath(destinationDirectory);
    mkdir(destinationDirectory + "/journal");

    let copyThread = copyBackupCursorExtendFiles(backupCursor, dbpath, destinationDirectory, async);
    return copyThread;
}

function copyBackupCursorExtendFiles(cursor, dbpath, destinationDirectory, async) {
    let files = _cursorToFiles(cursor);
    let copyThread;
    if (async) {
        copyThread = new Thread(_copyFiles, files, dbpath, destinationDirectory, _copyFileHelper);
        copyThread.start();
    } else {
        _copyFiles(files, dbpath, destinationDirectory, _copyFileHelper);
    }

    jsTestLog({
        msg: "Destination",
        destination: destinationDirectory,
        dbpath: ls(destinationDirectory),
        journal: ls(destinationDirectory + "/journal")
    });

    return copyThread;
}

function _cursorToFiles(cursor) {
    let files = [];
    while (cursor.hasNext()) {
        let doc = cursor.next();
        assert(doc.hasOwnProperty("filename"));
        files.push(doc.filename);
    }
    return files;
}

function _copyFiles(files, dbpath, destinationDirectory, copyFileHelper) {
    files.forEach((file) => {
        let dbgDoc = copyFileHelper(file, dbpath, destinationDirectory);
        dbgDoc["msg"] = "File copy";
        jsTestLog(dbgDoc);
    });
}

function _copyFileHelper(absoluteFilePath, sourceDbPath, destinationDirectory) {
    // Ensure the dbpath ends with an OS appropriate slash.
    let lastChar = sourceDbPath[sourceDbPath.length - 1];
    if (lastChar !== '/' && lastChar !== '\\') {
        if (_isWindows()) {
            sourceDbPath += '\\';
        } else {
            sourceDbPath += '/';
        }
    }

    // Ensure that the full path starts with the returned dbpath.
    assert.eq(0, absoluteFilePath.indexOf(sourceDbPath));

    // Grab the file path relative to the dbpath. Maintain that relation when copying
    // to the `hiddenDbpath`.
    let relativePath = absoluteFilePath.substr(sourceDbPath.length);
    let destination = destinationDirectory + '/' + relativePath;
    copyFile(absoluteFilePath, destination);
    return {fileSource: absoluteFilePath, relativePath: relativePath, fileDestination: destination};
}
