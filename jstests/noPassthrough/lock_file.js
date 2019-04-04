// The merizod process should always create a merizod.lock file in the data directory
// containing the process ID regardless of the storage engine requested.

(function() {
    // Ensures that merizod.lock exists and returns size of file.
    function getMerizodLockFileSize(dir) {
        var files = listFiles(dir);
        for (var i in files) {
            var file = files[i];
            if (!file.isDirectory && file.baseName == 'merizod.lock') {
                return file.size;
            }
        }
        assert(false, 'merizod.lock not found in data directory ' + dir);
    }

    var baseName = "jstests_lock_file";
    var dbpath = MerizoRunner.dataPath + baseName + '/';

    // Test framework will append --storageEngine command line option.
    var merizod = MerizoRunner.runMerizod({dbpath: dbpath});
    assert.neq(0,
               getMerizodLockFileSize(dbpath),
               'merizod.lock should not be empty while server is running');

    MerizoRunner.stopMerizod(merizod);

    // merizod.lock must be empty after shutting server down.
    assert.eq(
        0, getMerizodLockFileSize(dbpath), 'merizod.lock not truncated after shutting server down');
}());
