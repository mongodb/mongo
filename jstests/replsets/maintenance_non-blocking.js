// This test ensures that the replSetMaintenance command will not block, nor block-on, a db write
doTest = function() {
    "use strict";
    var replTest = new ReplSetTest({name: 'testSet', nodes: 2});
    var nodes = replTest.startSet();
    replTest.initiate();

    var m = replTest.getPrimary();
    var mColl = m.getDB("test").maint;
    var s = replTest.getSecondary();
    var sDB = s.getDB("test");
    var sColl = sDB.maint;

    var status = assert.commandWorked(sDB.adminCommand("replSetGetStatus"));
    printjson(status);

    print("******* fsyncLock'n secondary ************* ");
    s.getDB("admin").fsyncLock();

    // save some records
    var len = 100;
    for (var i = 0; i < len; ++i) {
        assert.writeOK(mColl.save({a: i}));
    }

    print("******* replSetMaintenance called on secondary ************* ");
    assert.commandWorked(sDB.adminCommand("replSetMaintenance"));

    var ismaster = assert.commandWorked(sColl.runCommand("ismaster"));
    assert.eq(false, ismaster.ismaster);
    assert.eq(false, ismaster.secondary);

    print("******* writing to primary ************* ");
    assert.writeOK(mColl.save({_id: -1}));
    printjson(sDB.currentOp());
    assert.neq(null, mColl.findOne());

    var ismaster = assert.commandWorked(sColl.runCommand("ismaster"));
    assert.eq(false, ismaster.ismaster);
    assert.eq(false, ismaster.secondary);

    print("******* fsyncUnlock'n secondary ************* ");
    sDB.fsyncUnlock();
};

doTest();
print("SUCCESS");
