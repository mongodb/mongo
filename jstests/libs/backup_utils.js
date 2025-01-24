load("jstests/libs/parallelTester.js");  // for Thread.

function backupData(mongo, destinationDirectory) {
    let backupCursor = openBackupCursor(mongo);
    let metadata = getBackupCursorMetadata(backupCursor);
    copyBackupCursorFiles(
        backupCursor, /*namespacesToSkip=*/[], metadata.dbpath, destinationDirectory);
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
function copyBackupCursorFiles(
    backupCursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback) {
    resetDbpath(destinationDirectory);
    let separator = _isWindows() ? '\\' : '/';
    // TODO(SERVER-13455): Replace `journal/` with the configurable journal path.
    mkdir(destinationDirectory + separator + "journal");

    let copyThread = copyBackupCursorExtendFiles(
        backupCursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback);
    return copyThread;
}

function copyBackupCursorFilesForIncremental(
    backupCursor, namespacesToSkip, dbpath, destinationDirectory) {
    // Remove any existing journal files from previous incremental backups.
    resetDbpath(destinationDirectory + "/journal");
    return copyBackupCursorExtendFiles(
        backupCursor, namespacesToSkip, dbpath, destinationDirectory, /*async=*/true);
}

function copyBackupCursorExtendFiles(
    cursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback) {
    let files = _cursorToFiles(cursor, namespacesToSkip, fileCopiedCallback);
    let copyThread;
    if (async) {
        copyThread = new Thread(_copyFiles, files, dbpath, destinationDirectory, _copyFileHelper);
        copyThread.start();
    } else {
        _copyFiles(files, dbpath, destinationDirectory, _copyFileHelper);
    }

    // TODO(SERVER-13455): Replace `journal/` with the configurable journal path.
    jsTestLog({
        msg: "Destination",
        destination: destinationDirectory,
        dbpath: ls(destinationDirectory),
        journal: ls(destinationDirectory + "/journal")
    });

    return copyThread;
}

function _cursorToFiles(cursor, namespacesToSkip, fileCopiedCallback) {
    let files = [];
    while (cursor.hasNext()) {
        let doc = cursor.next();
        assert(doc.hasOwnProperty("filename"));

        if (namespacesToSkip.includes(doc.ns)) {
            jsTestLog("Skipping file during backup: " + tojson(doc));
            continue;
        }

        if (fileCopiedCallback) {
            fileCopiedCallback(doc);
        }

        let file = {filename: doc.filename};

        if (doc.hasOwnProperty("fileSize")) {
            file.fileSize = Number(doc.fileSize);
        }

        if (doc.hasOwnProperty("offset")) {
            assert(doc.hasOwnProperty("length"));
            file.offset = Number(doc.offset);
            file.length = Number(doc.length);
        }

        files.push(file);
    }
    return files;
}

function _copyFiles(files, dbpath, destinationDirectory, copyFileHelper) {
    files.forEach((file) => {
        jsTestLog(copyFileHelper(file, dbpath, destinationDirectory));
    });
}

function _copyFileHelper(file, sourceDbPath, destinationDirectory) {
    let absoluteFilePath = file.filename;

    // Ensure the dbpath ends with an OS appropriate slash.
    let separator = '/';
    if (_isWindows()) {
        separator = '\\';
        // Convert dbpath which may contain directoryperdb/wiredTigerDirectoryForIndexes
        // subdirectory in POSIX style.
        absoluteFilePath = absoluteFilePath.replace(/[\/]/g, separator);
    }
    let lastChar = sourceDbPath[sourceDbPath.length - 1];
    if (lastChar !== '/' && lastChar !== '\\') {
        sourceDbPath += separator;
    }

    // Ensure that the full path starts with the returned dbpath.
    assert.eq(0, absoluteFilePath.indexOf(sourceDbPath));

    // Grab the file path relative to the dbpath. Maintain that relation when copying
    // to the `hiddenDbpath`.
    let relativePath = absoluteFilePath.substr(sourceDbPath.length);
    let destination = destinationDirectory + separator + relativePath;
    const newFileDirectory = destination.substring(0, destination.lastIndexOf(separator));
    mkdir(newFileDirectory);

    let msg = "File copy";
    if (!pathExists(destination)) {
        // If the file hasn't had an initial backup yet, then a full file copy is needed.
        copyFile(absoluteFilePath, destination);
    } else if (file.fileSize == undefined || file.length == undefined ||
               file.fileSize == file.length) {
        // - $backupCursorExtend, which only returns journal files does not report a 'fileSize'.
        // - 'length' is only reported for incremental backups.
        // - When 'fileSize' == 'length', that's used as an indicator to do a full file copy. Mostly
        // used for internal tables.
        if (pathExists(destination)) {
            // Remove the old backup of the file. For journal files specifically, if a checkpoint
            // didn't take place between two incremental backups, then the backup cursor can specify
            // journal files we've already copied at an earlier time. We should remove these old
            // journal files so that we can copy them over again in the event that their contents
            // have changed over time.
            jsTestLog(`Removing existing file ${
                destination} in preparation of copying a newer version of it`);
            removeFile(destination);
        }

        copyFile(absoluteFilePath, destination);
    } else {
        assert(file.offset != undefined);
        assert(file.length != undefined);
        msg = "Range copy, offset: " + file.offset + ", length: " + file.length;
        _copyFileRange(
            absoluteFilePath, destination, NumberLong(file.offset), NumberLong(file.length));
    }

    return {
        fileSource: absoluteFilePath,
        relativePath: relativePath,
        fileDestination: destination,
        msg: msg
    };
}
