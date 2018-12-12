load("jstests/libs/parallelTester.js");  // for ScopedThread.

function backupData(mongo, destinationDirectory) {
    let backupCursor = openBackupCursor(mongo);
    let res = copyBackupCursorFiles(backupCursor, destinationDirectory);
    backupCursor.close();
    return res.metadata;
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
        [{$backupCursorExtend: {backupId: backupId, timestamp: extendTo}}], {maxTimeMS: 10000});
}

/**
 * Exhaust the backup cursor and copy all the listed files to the destination directory. If `async`
 * is true, this function will spawn a ScopedThread doing the copy work and return the thread along
 * with the backup cursor metadata. The caller should `join` the thread when appropriate.
 */
function copyBackupCursorFiles(backupCursor, destinationDirectory, async) {
    resetDbpath(destinationDirectory);
    mkdir(destinationDirectory + "/journal");

    assert(backupCursor.hasNext());
    let doc = backupCursor.next();
    assert(doc.hasOwnProperty("metadata"));
    let metadata = doc["metadata"];

    let copyThread =
        copyBackupCursorExtendFiles(backupCursor, metadata["dbpath"], destinationDirectory, async);

    return {"metadata": metadata, "copyThread": copyThread};
}

function copyBackupCursorExtendFiles(cursor, dbpath, destinationDirectory, async) {
    let files = _cursorToFiles(cursor);
    let copyThread;
    if (async) {
        copyThread =
            new ScopedThread(_copyFiles, files, dbpath, destinationDirectory, _copyFileHelper);
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
