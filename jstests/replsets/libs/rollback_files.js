/**
 * Verifies that the rollback file for a given database path and namespace exists and contains the
 * 'expectedDocs', in any order. If there are multiple rollback files for the given collection,
 * chooses one of those files arbitrarily to read data from. Note that a rollback file is simply a
 * sequence of concatenated BSON objects, which is a format that can be read by the bsondump tool.
 */
function checkRollbackFiles(dbPath, nss, uuid, expectedDocs) {
    load("jstests/libs/uuid_util.js");

    // Check the path of the rollback directory.
    const rollbackDir = dbPath + '/rollback';
    assert(pathExists(rollbackDir), 'directory for rollback files does not exist: ' + rollbackDir);

    // We try to handle both possible rollback file layouts here. The first layout, used by the
    // older 'rollbackViaRefetch' algorithm, puts rollback files directly inside the /rollback
    // directory with a naming scheme of '<db>.<collection>.<timestamp>.bson'. The newer layout,
    // used by recover-to-timestamp (RTT) rollback, places them inside a
    // 'rollback/<db>.<collection>' directory with a file naming scheme of
    // 'removed.<timestamp>.bson'. The data formats of the files themselves should be the same in
    // both cases, though. These file layouts are documented here:
    // https://docs.mongodb.com/manual/core/replica-set-rollbacks/#collect-rollback-data.

    function getRollbackViaRefetchRollbackFile() {
        let files = listFiles(rollbackDir);
        let rollbackFiles =
            files.filter(f => !f.isDirectory && f.baseName.startsWith(extractUUIDFromObject(uuid)));
        assert.gte(rollbackFiles.length,
                   1,
                   "No rollbackViaRefetch rollback files found for namespace: " + nss +
                       " with UUID: " + uuid);
        return rollbackFiles[0].name;
    }

    function getRTTRollbackFile() {
        let rollbackFiles = listFiles(rollbackDir + "/" + extractUUIDFromObject(uuid));
        assert.gte(rollbackFiles.length,
                   1,
                   "No RTT rollback files found for namespace: " + nss + " with UUID: " + uuid);
        return rollbackFiles[0].name;
    }

    // If all the objects in the rollback directory are files, not directories, then this implies
    // the rollback files have been written using the rollbackViaRefetch mechanism. Otherwise, we
    // assume the files are written using the RTT mechanism.
    let rollbackFile;
    if (listFiles(rollbackDir).every(f => !f.isDirectory)) {
        print("Assuming rollback files written using the 'rollbackViaRefetch' layout.");
        rollbackFile = getRollbackViaRefetchRollbackFile();
    } else {
        print("Assuming rollback files written using the 'RTT' layout.");
        rollbackFile = getRTTRollbackFile();
    }

    print("Found rollback file: " + rollbackFile);

    // If the rollback BSON file is encrypted, don't try to check the data contents. Checking its
    // existence is sufficient.
    if (rollbackFile.endsWith(".enc")) {
        print("Bypassing check of rollback file data since it is encrypted.");
        return;
    }

    // Parse the BSON rollback file and check for the right
    // documents. The documents may be written out in an arbitrary
    // order so we just check the document set.
    //
    // Users are expected to consume these files with
    // bsondump. However, we use a shell builtin do do this testing so
    // that we don't have a dependency on the go tools in order to
    // test the server.
    const rollbackContents = _readDumpFile(rollbackFile);
    assert.sameMembers(rollbackContents, expectedDocs);
}
