// The bongod process should always create a bongod.lock file in the data directory
// containing the process ID regardless of the storage engine requested.

(function() {
    // Ensures that bongod.lock exists and returns size of file.
    function getBongodLockFileSize(dir) {
        var files = listFiles(dir);
        for (var i in files) {
            var file = files[i];
            if (!file.isDirectory && file.baseName == 'bongod.lock') {
                return file.size;
            }
        }
        assert(false, 'bongod.lock not found in data directory ' + dir);
    }

    var baseName = "jstests_lock_file";
    var dbpath = BongoRunner.dataPath + baseName + '/';

    // Test framework will append --storageEngine command line option if provided to smoke.py.
    var bongod = BongoRunner.runBongod({dbpath: dbpath, smallfiles: ""});
    assert.neq(0,
               getBongodLockFileSize(dbpath),
               'bongod.lock should not be empty while server is running');

    BongoRunner.stopBongod(bongod);

    // bongod.lock must be empty after shutting server down.
    assert.eq(
        0, getBongodLockFileSize(dbpath), 'bongod.lock not truncated after shutting server down');
}());
