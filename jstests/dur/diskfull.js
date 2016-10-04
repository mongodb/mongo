/** Test running out of disk space with durability enabled.
To set up the test, it's required to set up a small partition something like the following:
sudo umount /data/db/diskfulltest/
rm -rf /data/db/diskfulltest
mkdir -p /data/images
dd bs=512 count=83968 if=/dev/zero of=/data/images/diskfulltest.img
/sbin/mkfs.ext2 -m 0 -F /data/images/diskfulltest.img
mkdir -p /data/db/diskfulltest
mount -o loop /data/images/diskfulltest.img /data/db/diskfulltest
*/

startPath = MongoRunner.dataDir + "/diskfulltest";
recoverPath = MongoRunner.dataDir + "/dur_diskfull";

doIt = false;
files = listFiles(MongoRunner.dataDir);
for (i in files) {
    if (files[i].name == startPath) {
        doIt = true;
    }
}

if (!doIt) {
    print("path " + startPath + " missing, skipping diskfull test");
    doIt = false;
}

function checkNoJournalFiles(path, pass) {
    var files = listFiles(path);
    if (files.some(function(f) {
            return f.name.indexOf("prealloc") < 0;
        })) {
        if (pass == null) {
            // wait a bit longer for mongod to potentially finish if it is still running.
            sleep(10000);
            return checkNoJournalFiles(path, 1);
        }
        print("\n\n\n");
        print("FAIL path:" + path);
        print("unexpected files:");
        printjson(files);
        assert(false, "FAIL a journal/lsn file is present which is unexpected");
    }
}

/** Clear dbpath without removing and recreating diskfulltest directory, as resetDbpath does */
function clear() {
    files = listFiles(startPath);
    files.forEach(function(x) {
        removeFile(x.name);
    });
}

function log(str) {
    print();
    if (str)
        print(testname + " step " + step++ + " " + str);
    else
        print(testname + " step " + step++);
}

function work() {
    log("work");
    try {
        var d = conn.getDB("test");
        var big = new Array(5000).toString();
        var bulk = d.foo.initializeUnorderedBulkOp();
        // This part of the test depends on the partition size used in the build env
        // Currently, unused, but with larger partitions insert enough documents here
        // to create a second db file
        for (i = 0; i < 1; ++i) {
            bulk.insert({_id: i, b: big});
        }
        assert.writeOK(bulk.execute());
    } catch (e) {
        print(e);
        raise(e);
    } finally {
        log("endwork");
    }
}

function verify() {
    log("verify");
    var d = conn.getDB("test");
    c = d.foo.count();
    v = d.foo.validate();
    // not much we can guarantee about the writes, just validate when possible
    if (c != 0 && !v.valid) {
        printjson(v);
        print(c);
        assert(v.valid);
        assert.gt(c, 0);
    }
}

function runFirstMongodAndFillDisk() {
    log();

    clear();
    conn = MongoRunner.runMongod({
        restart: true,
        cleanData: false,
        dbpath: startPath,
        journal: "",
        smallfiles: "",
        journalOptions: 8 + 64,
        noprealloc: ""
    });

    assert.throws(work, null, "no exception thrown when exceeding disk capacity");
    MongoRunner.stopMongod(conn);

    sleep(5000);
}

function runSecondMongdAndRecover() {
    // restart and recover
    log();
    conn = MongoRunner.runMongod({
        restart: true,
        cleanData: false,
        dbpath: startPath,
        journal: "",
        smallfiles: "",
        journalOptions: 8 + 64,
        noprealloc: ""
    });
    verify();

    log("stop");
    MongoRunner.stopMongod(conn);

    // stopMongod seems to be asynchronous (hmmm) so we sleep here.
    sleep(5000);

    // at this point, after clean shutdown, there should be no journal files
    log("check no journal files");
    checkNoJournalFiles(startPath + "/journal/");

    log();
}

function someWritesInJournal() {
    runFirstMongodAndFillDisk();
    runSecondMongdAndRecover();
}

function noWritesInJournal() {
    // It is too difficult to consistently trigger cases where there are no existing journal files
    // due to lack of disk space, but
    // if we were to test this case we would need to manualy remove the lock file.
    //    removeFile( startPath + "/mongod.lock" );
}

if (doIt) {
    var testname = "dur_diskfull";
    var step = 1;
    var conn = null;

    someWritesInJournal();
    noWritesInJournal();

    print(testname + " SUCCESS");
}
