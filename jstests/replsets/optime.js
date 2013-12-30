// Tests tracking of latestOptime and earliestOptime in serverStatus.oplog

function optimesAreEqual(replTest) {
    var prevStatus = replTest.nodes[0].getDB('admin').serverStatus({oplog:true}).oplog;
    for (var i = 1; i < replTest.nodes.length; i++) {
        var status = replTest.nodes[i].getDB('admin').serverStatus({oplog:true}).oplog;
        if (!friendlyEqual(prevStatus.latestOptime, status.latestOptime) ||
            !friendlyEqual(prevStatus.earliestOptime, status.earliestOptime)) {
            return false;
        }
        prevStatus = status;
    }
    return true;
}

var replTest = new ReplSetTest( { name : "replStatus" , nodes: 3, oplogSize: 1 } );

replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

// Check initial optimes
assert(optimesAreEqual(replTest));
var initialInfo = master.getDB('admin').serverStatus({oplog:true}).oplog;

// Do an insert to increment optime, but without rolling the oplog
// latestOptime should be updated, but earliestOptime should be unchanged
master.getDB('test').foo.insert({a:1});
master.getDB('test').getLastError(replTest.nodes.length);
assert(optimesAreEqual(replTest));

var info = master.getDB('admin').serverStatus({oplog:true}).oplog;
assert.gt(info.latestOptime, initialInfo.latestOptime);
assert.eq(info.earliestOptime, initialInfo.earliestOptime);

// Insert some large documents to force the oplog to roll over
var largeString = new Array(1024*100).toString();
for (var i = 0; i < 15; i++) {
    master.getDB('test').foo.insert({largeString: largeString});
    master.getDB('test').getLastError(replTest.nodes.length);
}
assert(optimesAreEqual(replTest));

// Test that earliestOptime was updated
info = master.getDB('admin').serverStatus({oplog:true}).oplog;
assert.gt(info.latestOptime, initialInfo.latestOptime);
assert.gt(info.earliestOptime, initialInfo.earliestOptime);

replTest.stopSet();