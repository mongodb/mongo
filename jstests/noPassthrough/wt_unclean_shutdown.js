/**
 * This test is only for the WiredTiger storage engine. Test to reproduce recovery bugs in WT from
 * WT-2696 and WT-2706.  Have several threads inserting unique data.  Kill -9 mongod.  After
 * restart and recovery verify that all expected records inserted are there and no records in the
 * middle of the data set are lost.
 *
 * @tags: [requires_wiredtiger]
 */

load('jstests/libs/parallelTester.js');  // For Thread

(function() {
'use strict';

// Skip this test if not running with the "wiredTiger" storage engine.
if (jsTest.options().storageEngine && jsTest.options().storageEngine !== 'wiredTiger') {
    jsTest.log('Skipping test because storageEngine is not "wiredTiger"');
    return;
}

var dbpath = MongoRunner.dataPath + 'wt_unclean_shutdown';
resetDbpath(dbpath);

var conn = MongoRunner.runMongod({
    dbpath: dbpath,
    noCleanData: true,
    // Modify some WT settings:
    // - Disable checkpoints based on log size so that we know no checkpoint gets written.
    // - Explicitly set checkpoints to 60 seconds in case the default ever changes.
    // - Turn off archiving and compression for easier debugging if there is a failure.
    // - Make the maximum file size small to encourage lots of file changes.  WT-2706 was
    // related to log file switches.
    wiredTigerEngineConfigString:
        'checkpoint=(wait=60,log_size=0),log=(remove=false,compressor=none,file_max=10M)'
});
assert.neq(null, conn, 'mongod was unable to start up');

var insertWorkload = function(host, start, end) {
    var conn = new Mongo(host);
    var testDB = conn.getDB('test');

    // Create a record larger than 128K which is the threshold to doing an unbuffered log
    // write in WiredTiger.
    var largeString = 'a'.repeat(1024 * 128);

    for (var i = start; i < end; i++) {
        var doc = {_id: i, x: 0};
        // One of the bugs, WT-2696, was related to large records that used the unbuffered
        // log code.  Periodically insert the large record to stress that code path.
        if (i % 30 === 0) {
            doc.x = largeString;
        }

        try {
            testDB.coll.insert(doc);
        } catch (e) {
            // Terminate the loop when mongod is killed.
            break;
        }
    }
    // Return i, the last record we were trying to insert.  It is possible that mongod gets
    // killed in the middle but not finding a record at the end is okay.  We're only
    // interested in records missing in the middle.
    return {start: start, end: i};
};

// Start the insert workload threads with partitioned input spaces.
// We don't run long enough for threads to overlap.  Adjust the per thread value if needed.
var max_per_thread = 1000000;
var num_threads = 8;
var threads = [];
for (var i = 0; i < num_threads; i++) {
    var t = new Thread(
        insertWorkload, conn.host, i * max_per_thread, max_per_thread + (i * max_per_thread));
    threads.push(t);
    t.start();
}

// Sleep for sometime less than a minute so that mongod has not yet written a checkpoint.
// That will force WT to run recovery all the way from the beginning and we can detect missing
// records.  Sleep for 40 seconds to generate plenty of workload.
sleep(40000);

// Mongod needs an unclean shutdown so that WT recovery is forced on restart and we can detect
// any missing records.
MongoRunner.stopMongod(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

// Retrieve the start and end data from each thread.
var retData = [];
threads.forEach(function(t) {
    t.join();
    retData.push(t.returnData());
});

// Restart the mongod.  This forces WT to run recovery.
conn = MongoRunner.runMongod({
    dbpath: dbpath,
    noCleanData: true,
    wiredTigerEngineConfigString: 'log=(remove=false,compressor=none,file_max=10M)'
});
assert.neq(null, conn, 'mongod should have restarted');

// Verify that every item between start and end for every thread exists in the collection now
// that recovery has completed.
var coll = conn.getDB('test').coll;
for (var i = 0; i < retData.length; i++) {
    // For each start and end, verify every data item exists.
    var thread_data = retData[i];
    var absent = null;
    var missing = null;
    for (var j = thread_data.start; j <= thread_data.end; j++) {
        var idExists = coll.find({_id: j}).count() > 0;
        // The verification is a bit complex.  We only want to fail if records in the middle
        // of the range are missing.  Records at the end may be missing due to when mongod
        // was killed and records in memory are lost.  It is only a bug if a record is missing
        // and a subsequent record exists.
        if (!idExists) {
            absent = j;
        } else if (absent !== null) {
            missing = absent;
            break;
        }
    }
    assert.eq(null,
              missing,
              'Thread ' + i + ' missing id ' + missing +
                  ' start and end for all threads: ' + tojson(retData));
}

MongoRunner.stopMongod(conn);
})();
