// SERVER-2351 Test killop with repair command.
(function() {
    'use strict';
    var baseName = "jstests_disk_repair5";

    var dbpath = MongoRunner.dataPath + baseName + "/";
    var repairpath = dbpath + "repairDir/";

    resetDbpath(dbpath);
    resetDbpath(repairpath);

    var m = MongoRunner.runMongod({
        dbpath: dbpath,
        repairpath: repairpath,
        restart: true,
        cleanData: false
    });  // So that the repair dir won't get removed

    var dbTest = m.getDB(baseName);

    // Insert a lot of data so repair runs a long time
    var bulk = dbTest[baseName].initializeUnorderedBulkOp();
    var big = new Array(5000).toString();
    for (var i = 0; i < 20000; ++i) {
        bulk.insert({i: i, b: big});
    }
    assert.writeOK(bulk.execute());

    function killRepair() {
        while (1) {
            var p = db.currentOp().inprog;
            for (var i in p) {
                var o = p[i];
                printjson(o);

                // Find the active 'repairDatabase' op and kill it.
                if (o.active && o.query && o.query.repairDatabase) {
                    db.killOp(o.opid);
                    return;
                }
            }
        }
    }

    var s = startParallelShell(killRepair.toString() + "; killRepair();", m.port);
    sleep(100);  // make sure shell is actually running, lame

    // Repair should fail due to killOp.
    assert.commandFailed(dbTest.runCommand({repairDatabase: 1, backupOriginalFiles: true}));

    s();

    assert.eq(20000, dbTest[baseName].find().itcount());
    assert(dbTest[baseName].validate().valid);

    MongoRunner.stopMongod(m);
})();
