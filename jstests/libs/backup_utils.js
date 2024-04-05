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
    mkdir(destinationDirectory + "/journal");

    let copyThread = copyBackupCursorExtendFiles(
        backupCursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback);
    return copyThread;
}

export function copyBackupCursorExtendFiles(
    cursor, namespacesToSkip, dbpath, destinationDirectory, async, fileCopiedCallback) {
    let files = _cursorToFiles(cursor, namespacesToSkip, fileCopiedCallback);
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

        files.push(doc.filename);
    }
    return files;
}

export function _copyFiles(files, dbpath, destinationDirectory, copyFileHelper) {
    files.forEach((file) => {
        let dbgDoc = copyFileHelper(file, dbpath, destinationDirectory);
        dbgDoc["msg"] = "File copy";
        jsTestLog(dbgDoc);
    });
}

export function _copyFileHelper(absoluteFilePath, sourceDbPath, destinationDirectory) {
    // Ensure the dbpath ends with an OS appropriate slash.
    let separator = '/';
    if (_isWindows()) {
        separator = '\\';
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
    copyFile(absoluteFilePath, destination);
    return {fileSource: absoluteFilePath, relativePath: relativePath, fileDestination: destination};
}

// Magic restore utility functions

/**
 * Helper function that generates the magic restore named pipe path for testing. 'pipeDir'
 * is the directory to create the named pipe in the filesystem.
 */
function _generateMagicRestorePipePath(pipeDir) {
    const pipeName = "magic_restore_named_pipe";
    // On Windows, the pipe path prefix is ignored. "//./pipe/" is the required path start of all
    // named pipes on Windows.
    const pipePath = _isWindows() ? "//./pipe/" + pipeName : `${pipeDir}/tmp/${pipeName}`;
    if (!_isWindows() && !fileExists(pipeDir + "/tmp/")) {
        assert(mkdir(pipeDir + "/tmp/").created);
    }
    return {pipeName, pipePath};
}

/**
 * Helper function that writes an array of JavaScript objects into a named pipe. 'objs' will be
 * serialized into BSON and written into the named pipe path generated by
 * '_generateMagicRestorePipePath'.
 */
export function _writeObjsToMagicRestorePipe(objs, pipeDir, persistPipe = true) {
    const {pipeName, pipePath} = _generateMagicRestorePipePath(pipeDir);
    _writeTestPipeObjects(pipeName, objs.length, objs, pipeDir + "/tmp/", persistPipe);
    // Creating the named pipe is async, so we should wait until the file exists.
    assert.soon(() => fileExists(pipePath));
}

/**
 * Helper function that starts and completes a magic restore node on the provided 'backupDbPath'.
 */
export function _runMagicRestoreNode(backupDbPath, pipeDir, options = {}) {
    const {pipePath} = _generateMagicRestorePipePath(pipeDir);
    // Magic restore will exit the mongod process cleanly. 'runMongod' may acquire a connection to
    // mongod before it exits, and so we wait for the process to exit in the 'assert.soon' below. If
    // mongod exits before we acquire a connection, 'conn' will be null. In this case, if mongod
    // exits with non-zero exit code, the runner will throw a StopError.
    const conn = MongoRunner.runMongod({
        dbpath: backupDbPath,
        noCleanData: true,
        magicRestore: "",
        env: {namedPipeInput: pipePath},
        ...options
    });
    if (conn) {
        assert.soon(() => {
            const res = checkProgram(conn.pid);
            return !res.alive && res.exitCode == MongoRunner.EXIT_CLEAN;
        }, "Expected magic restore to exit mongod cleanly");
    }
}
