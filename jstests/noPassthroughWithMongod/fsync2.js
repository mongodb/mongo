
function debug(msg) {
    print("fsync2: " + msg);
}

var loops = 200;
if (db.getSisterDB("local").slaves.count() > 0) {
    // replication can cause some write locks on local
    // therefore this test is flaky with replication on
    loops = 1;
}

function doTest() {
    db.fsync2.drop();
    // Make write ops asynchronous so the test won't hang when in fsync lock mode.
    db.getMongo().forceWriteMode('legacy');

    db.fsync2.save({x: 1});

    d = db.getSisterDB("admin");

    // Don't test if the engine doesn't support fsyncLock
    var ret = d.runCommand({fsync: 1, lock: 1});
    if (!ret.ok) {
        assert.commandFailedWithCode(ret, ErrorCodes.CommandNotSupported);
        jsTestLog("Skipping test as engine does not support fsyncLock");
        return;
    }

    assert.commandWorked(ret);

    debug("after lock");

    for (var i = 0; i < loops; i++) {
        debug("loop: " + i);
        assert.eq(1, db.fsync2.count());
        sleep(100);
    }

    debug("about to save");
    db.fsync2.save({x: 1});
    debug("save done");

    m = new Mongo(db.getMongo().host);

    // Uncomment once SERVER-4243 is fixed
    // assert.eq(1, m.getDB(db.getName()).fsync2.count());

    assert(m.getDB("admin").fsyncUnlock().ok);

    assert.eq(2, db.fsync2.count());
}

if (!jsTest.options().auth) {  // SERVER-4243
    doTest();
}
