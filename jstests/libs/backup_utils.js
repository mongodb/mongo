import {Thread} from "jstests/libs/parallelTester.js";

export function backupData(mongo, destinationDirectory) {
    let backupCursor = openBackupCursor(mongo.getDB("admin"));
    let metadata = getBackupCursorMetadata(backupCursor);
    copyBackupCursorFiles(
        backupCursor, /*namespacesToSkip=*/[], metadata.dbpath, destinationDirectory);
    backupCursor.close();
    return metadata;
}

export function openBackupCursor(db, backupOptions, aggregateOptions) {
    // Opening a backup cursor can race with taking a checkpoint, resulting in a transient
    // error. Retry until it succeeds.
    backupOptions = backupOptions || {};
    aggregateOptions = aggregateOptions || {};

    while (true) {
        try {
            return db.aggregate([{$backupCursor: backupOptions}], aggregateOptions);
        } catch (exc) {
            jsTestLog({"Failed to open a backup cursor, retrying.": exc});
        }
    }
}

export function extendBackupCursor(mongo, backupId, extendTo) {
    return mongo.getDB("admin").aggregate(
        [{$backupCursorExtend: {backupId: backupId, timestamp: extendTo}}],
        {maxTimeMS: 180 * 1000});
}

export function startHeartbeatThread(host, backupCursor, session, stopCounter) {
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

    const heartbeater = new Thread(heartbeatBackupCursor, host, cursorId, lsid, stopCounter);
    heartbeater.start();
    return heartbeater;
}

export function getBackupCursorMetadata(backupCursor) {
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
export function copyBackupCursorFiles(
    backupCursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback) {
    resetDbpath(destinationDirectory);
    let separator = _isWindows() ? '\\' : '/';
    // TODO(SERVER-13455): Replace `journal/` with the configurable journal path.
    mkdir(destinationDirectory + separator + "journal");

    let copyThread = copyBackupCursorExtendFiles(
        backupCursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback);
    return copyThread;
}

export function copyBackupCursorFilesForIncremental(
    backupCursor, namespacesToSkip, dbpath, destinationDirectory) {
    // Remove any existing journal files from previous incremental backups.
    resetDbpath(destinationDirectory + "/journal");

    return copyBackupCursorExtendFiles(
        backupCursor, namespacesToSkip, dbpath, destinationDirectory, /*async=*/ true);
}

export function copyBackupCursorExtendFiles(
    cursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback) {
    let files = _cursorToFiles(cursor, namespacesToSkip, fileCopiedCallback);
    let copyThread;
    if (async) {
        copyThread = new Thread(_copyFiles, files, dbpath, destinationDirectory, copyFileHelper);
        copyThread.start();
    } else {
        _copyFiles(files, dbpath, destinationDirectory, copyFileHelper);
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

export function _cursorToFiles(cursor, namespacesToSkip, fileCopiedCallback) {
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

export function _copyFiles(files, dbpath, destinationDirectory, copyFileHelper) {
    files.forEach((file) => {
        jsTestLog(copyFileHelper(file, dbpath, destinationDirectory));
    });
}

export function copyFileHelper(file, sourceDbPath, destinationDirectory) {
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

/**
 * Helper function to ensure the namespace and UUID fields are correctly populated in files we are
 * backing up.
 */
export function checkBackup(backupCursor) {
    // Print the metadata document.
    assert(backupCursor.hasNext());
    jsTestLog(backupCursor.next());

    while (backupCursor.hasNext()) {
        let doc = backupCursor.next();

        jsTestLog("File for backup: " + tojson(doc));

        if (!doc.required) {
            assert.neq(doc.ns, "");
            assert.neq(doc.uuid, "");
        } else {
            let pathsep = _isWindows() ? '\\' : '/';
            let stem = doc.filename.substr(doc.filename.lastIndexOf(pathsep) + 1);
            // Denylisting internal files that don't need to have ns/uuid set. Denylisting known
            // patterns will help catch subtle API changes if new filename patterns are added that
            // don't generate ns/uuid.
            if (!stem.startsWith("size") && !stem.startsWith("Wired") && !stem.startsWith("_")) {
                assert.neq(doc.ns, "");
                assert.neq(doc.uuid, "");
            }
        }
    }
}
