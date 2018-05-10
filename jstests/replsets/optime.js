// Tests tracking of latestOptime and earliestOptime in serverStatus.oplog

function timestampCompare(o1, o2) {
    if (o1.t < o2.t) {
        return -1;
    } else if (o1.t > o2.t) {
        return 1;
    } else {
        if (o1.i < o2.i) {
            return -1;
        } else if (o1.i > o2.i) {
            return 1;
        } else {
            return 0;
        }
    }
}

function optimesAreEqual(replTest) {
    var prevStatus = replTest.nodes[0].getDB('admin').serverStatus({oplog: true}).oplog;
    for (var i = 1; i < replTest.nodes.length; i++) {
        var status = replTest.nodes[i].getDB('admin').serverStatus({oplog: true}).oplog;
        if (timestampCompare(prevStatus.latestOptime, status.latestOptime) != 0) {
            jsTest.log("optimesAreEqual returning false match, prevOptime: " +
                       prevStatus.latestOptime + " latestOptime: " + status.latestOptime);
            replTest.dumpOplog(replTest.nodes[i], {}, 20);
            replTest.dumpOplog(replTest.nodes[i - 1], {}, 20);
            return false;
        }
        prevStatus = status;
    }
    return true;
}

// This test has a part that rolls over the oplog. Doing so requires a fresh stable
// checkpoint. Set the syncdelay to a small value to increase checkpoint frequency.
var replTest = new ReplSetTest(
    {name: "replStatus", nodes: 3, oplogSize: 1, waitForKeys: true, nodeOptions: {syncdelay: 1}});

replTest.startSet();
replTest.initiate();
var master = replTest.getPrimary();
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

// Check initial optimes
assert(optimesAreEqual(replTest));
var initialInfo = master.getDB('admin').serverStatus({oplog: true}).oplog;

// Do an insert to increment optime, but without rolling the oplog
// latestOptime should be updated, but earliestOptime should be unchanged
var options = {writeConcern: {w: replTest.nodes.length}};
assert.writeOK(master.getDB('test').foo.insert({a: 1}, options));
assert(optimesAreEqual(replTest));

var info = master.getDB('admin').serverStatus({oplog: true}).oplog;
assert.gt(timestampCompare(info.latestOptime, initialInfo.latestOptime), 0);
assert.eq(timestampCompare(info.earliestOptime, initialInfo.earliestOptime), 0);

// Insert some large documents to force the oplog to roll over
var largeString = new Array(1024 * 10).toString();
for (var i = 0; i < 2000; i++) {
    master.getDB('test').foo.insert({largeString: largeString}, options);
}
assert.soon(function() {
    return optimesAreEqual(replTest);
});

// This block requires a fresh stable checkpoint.
assert.soon(function() {
    // Test that earliestOptime was updated
    info = master.getDB('admin').serverStatus({oplog: true}).oplog;
    return timestampCompare(info.latestOptime, initialInfo.latestOptime) > 0 &&
        timestampCompare(info.earliestOptime, initialInfo.earliestOptime) > 0;
});

replTest.stopSet();
