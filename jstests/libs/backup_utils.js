function backupData(mongo, destinationDirectory) {
    let backupCursor = openBackupCursor(mongo);
    let metadata = copyCursorFiles(mongo, backupCursor, destinationDirectory);
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

function copyCursorFiles(mongo, backupCursor, destinationDirectory) {
    resetDbpath(destinationDirectory);
    mkdir(destinationDirectory + "/journal");

    assert(backupCursor.hasNext());
    let doc = backupCursor.next();
    assert(doc.hasOwnProperty("metadata"));
    let metadata = doc["metadata"];

    while (backupCursor.hasNext()) {
        let doc = backupCursor.next();
        assert(doc.hasOwnProperty("filename"));
        let dbgDoc = copyFileHelper(doc["filename"], metadata["dbpath"], destinationDirectory);
        dbgDoc["msg"] = "File copy";
        jsTestLog(dbgDoc);
    }

    jsTestLog({
        msg: "Destination",
        destination: destinationDirectory,
        dbpath: ls(destinationDirectory),
        journal: ls(destinationDirectory + "/journal")
    });

    return metadata;
}

function copyFileHelper(absoluteFilePath, sourceDbPath, destinationDirectory) {
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
