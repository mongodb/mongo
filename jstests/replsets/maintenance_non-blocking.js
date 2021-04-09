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

    // The default WC is majority and fsyncLock will prevent satisfying any majority writes.
    assert.commandWorked(replTest.getPrimary().adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

    var status = assert.commandWorked(sDB.adminCommand("replSetGetStatus"));
    printjson(status);

    print("******* fsyncLock'n secondary ************* ");
    s.getDB("admin").fsyncLock();

    // save some records
    var len = 100;
    for (var i = 0; i < len; ++i) {
        assert.commandWorked(mColl.save({a: i}));
    }

    print("******* replSetMaintenance called on secondary ************* ");
    assert.commandWorked(sDB.adminCommand("replSetMaintenance"));

    var hello = assert.commandWorked(sColl.runCommand("hello"));
    assert.eq(false, hello.isWritablePrimary);
    assert.eq(false, hello.secondary);

    print("******* writing to primary ************* ");
    assert.commandWorked(mColl.save({_id: -1}));
    assert.neq(null, mColl.findOne());

    var hello = assert.commandWorked(sColl.runCommand("hello"));
    assert.eq(false, hello.isWritablePrimary);
    assert.eq(false, hello.secondary);

    print("******* fsyncUnlock'n secondary ************* ");
    sDB.fsyncUnlock();

    print("******* unset replSetMaintenance on secondary ************* ");
    assert.commandWorked(sDB.adminCommand({replSetMaintenance: 0}));
    replTest.stopSet();
};

doTest();
print("SUCCESS");
