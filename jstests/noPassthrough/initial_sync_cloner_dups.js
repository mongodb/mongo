/**
 * Test for SERVER-17487
 * 3 node replset 
 * insert docs with numeric _ids
 * start deleting/re-inserting docs from collection in a loop
 * add new secondary to force initialSync
 * verify collection and both indexes on the secondary have the right number of docs
 */
(function() {
'use strict';
load('jstests/libs/parallelTester.js');

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
}

var replTest = new ReplSetTest({name: 'cloner', nodes: 3});
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
var numDocs = 100*1000;
var bigStr = Array(1001).toString();
var batch = coll.initializeUnorderedBulkOp();
for (var i=0; i < numDocs; i++) {
    batch.insert({_id: i, bigStr: bigStr});
}
batch.execute();

replTest.awaitReplication(2*60*1000);

jsTestLog("Start remove/insert on primary");
var insertAndRemove = function(host) {
    jsTestLog("starting bg writes on " + host);
    var m = new Mongo(host);
    var db = m.getDB('test');
    var coll = db.cloner;
    var numDocs = coll.count();
    for (var i=0; !db.stop.findOne(); i++) {
            var id = Random.randInt(numDocs);
            coll.remove({_id: id});
            coll.insert({_id: id});

            var id = i % numDocs;
            //print(id);
            coll.remove({_id: id});
            coll.insert({_id: id});
    }

    jsTestLog("finished bg writes on " + host);
}
var worker = new ScopedThread(insertAndRemove, primary.host);
worker.start();

jsTestLog("add a new secondary");
var secondary = replTest.add({});
replTest.reInitiate();
secondary.setSlaveOk();

// NOTE: This is here to prevent false negatives, but it is racy and dependent on magic numbers.
// Removed the assertion because it was too flaky.  Printing a warning instead (dan)
jsTestLog("making sure we dropped some dups");
var res = secondary.adminCommand({getLog:"global"});
var droppedDups = (contains(res.log, function(v) {
    return v.indexOf("index build dropped"/* NNN dups*/) != -1;
}));
if (!droppedDups) {
    jsTestLog("Warning: Test did not trigger duplicate documents, this run will be a false negative");
}

jsTestLog("stoping writes and waiting for replica set to coalesce")
primary.getDB('test').stop.insert({});
worker.join();
replTest.awaitReplication(); // Make sure all writes have hit secondary.

jsTestLog("check that secondary has correct counts");
var secondaryColl = secondary.getDB('test').getCollection('cloner');
var index = secondaryColl.find({},{_id:1}).hint({_id:1}).itcount();
var secondary_index = secondaryColl.find({},{_id:1}).hint({k:1}).itcount();
var table = secondaryColl.find({},{_id:1}).hint({$natural:1}).itcount();
if (index != table || index != secondary_index) {
    printjson({name: coll,
              _id_index_count:index,
              secondary_index_count: secondary_index,
              table_count: table});
}
assert.eq(index, table) ;
assert.eq(table, secondary_index);
})();
