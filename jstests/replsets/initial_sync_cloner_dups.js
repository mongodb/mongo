/**
 * Test for SERVER-17487
 * 3 node replset
 * insert docs with numeric _ids
 * start deleting/re-inserting docs from collection in a loop
 * add new secondary to force initialSync
 * verify collection and both indexes on the secondary have the right number of docs
 */
(function(doNotRun) {
    "use strict";

    if (doNotRun) {
        return;
    }

    load('jstests/libs/parallelTester.js');

    Random.setRandomSeed();

    // used to parse RAM log file
    var contains = function(logLines, func) {
        var i = logLines.length;
        while (i--) {
            printjson(logLines[i]);
            if (func(logLines[i])) {
                return true;
            }
        }
        return false;
    };

    var replTest = new ReplSetTest({name: 'cloner', nodes: 3, oplogSize: 150 /*~1.5x data size*/});
    replTest.startSet();
    var conf = replTest.getReplSetConfig();
    conf.settings = {};
    conf.settings.chainingAllowed = false;
    replTest.initiate(conf);
    replTest.awaitSecondaryNodes();
    var primary = replTest.getPrimary();
    var coll = primary.getDB('test').cloner;
    coll.drop();
    coll.createIndex({k: 1});

    // These need to be big enough to force initial-sync to use many batches
    var numDocs = 100 * 1000;
    var bigStr = Array(1001).toString();
    var batch = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        batch.insert({_id: i, bigStr: bigStr});
    }
    batch.execute();

    replTest.awaitReplication();

    jsTestLog("Start remove/insert on primary");
    var insertAndRemove = function(host) {
        jsTestLog("starting bg writes on " + host);
        var m = new Mongo(host);
        var db = m.getDB('test');
        var coll = db.cloner;
        var numDocs = coll.count();
        for (var i = 0; !db.stop.findOne(); i++) {
            var id = Random.randInt(numDocs);
            coll.remove({_id: id});
            coll.insert({_id: id});

            var id = i % numDocs;
            // print(id);
            coll.remove({_id: id});
            coll.insert({_id: id});

            // Try to throttle this thread to prevent overloading slow machines.
            sleep(1);
        }

        jsTestLog("finished bg writes on " + host);
    };
    var worker = new ScopedThread(insertAndRemove, primary.host);
    worker.start();

    jsTestLog("add a new secondary");
    var secondary = replTest.add({});
    replTest.reInitiate();
    secondary.setSlaveOk();
    // Wait for the secondary to get ReplSetInitiate command.
    replTest.waitForState(
        secondary,
        [ReplSetTest.State.STARTUP_2, ReplSetTest.State.RECOVERING, ReplSetTest.State.SECONDARY]);

    // This fail point will cause the first intial sync to fail, and leave an op in the buffer to
    // verify the fix from SERVER-17807
    print("=================== failpoint enabled ==============");
    printjson(assert.commandWorked(secondary.getDB("admin").adminCommand(
        {configureFailPoint: 'failInitSyncWithBufferedEntriesLeft', mode: {times: 1}})));
    printjson(assert.commandWorked(secondary.getDB("admin").adminCommand({resync: true})));

    // NOTE: This is here to prevent false negatives, but it is racy and dependent on magic numbers.
    // Removed the assertion because it was too flaky.  Printing a warning instead (dan)
    jsTestLog("making sure we dropped some dups");
    var res = secondary.adminCommand({getLog: "global"});
    var droppedDups = (contains(res.log, function(v) {
        return v.indexOf("index build dropped" /* NNN dups*/) != -1;
    }));
    if (!droppedDups) {
        jsTestLog(
            "Warning: Test did not trigger duplicate documents, this run will be a false negative");
    }

    jsTestLog("stopping writes and waiting for replica set to coalesce");
    primary.getDB('test').stop.insert({});
    worker.join();
    // make sure all secondaries are caught up, after init sync
    reconnect(secondary.getDB("test"));
    replTest.awaitSecondaryNodes();
    replTest.awaitReplication();

    jsTestLog("check that secondary has correct counts");
    var secondaryColl = secondary.getDB('test').getCollection('cloner');
    var index = secondaryColl.find({}, {_id: 1}).hint({_id: 1}).itcount();
    var secondary_index = secondaryColl.find({}, {_id: 1}).hint({k: 1}).itcount();
    var table = secondaryColl.find({}, {_id: 1}).hint({$natural: 1}).itcount();
    if (index != table || index != secondary_index) {
        printjson({
            name: coll,
            _id_index_count: index,
            secondary_index_count: secondary_index,
            table_count: table
        });
    }
    assert.eq(index, table);
    assert.eq(table, secondary_index);
})(true /* Disabled until SERVER-23476 re-enabled rsync command */);
