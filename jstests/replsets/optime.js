// Tests tracking of latestOptime and earliestOptime in serverStatus.oplog
// Also tests tracking of wall clock times in replSetGetStatus

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

function wallTimeCompare(d1, d2) {
    if (d1 < d2) {
        return -1;
    } else if (d1 > d2) {
        return 1;
    } else {
        return 0;
    }
}

function optimesAndWallTimesAreEqual(replTest) {
    let prevReplStatus = replTest.nodes[0].getDB('admin').runCommand({replSetGetStatus: 1});
    let prevOptime = prevReplStatus.optimes.appliedOpTime.ts;
    let prevAppliedWallTime = prevReplStatus.optimes.lastAppliedWallTime;
    let prevDurableWallTime = prevReplStatus.optimes.lastDurableWallTime;
    for (var i = 1; i < replTest.nodes.length; i++) {
        let currentReplStatus = replTest.nodes[i].getDB('admin').runCommand({replSetGetStatus: 1});
        let currOptime = currentReplStatus.optimes.appliedOpTime.ts;
        let currAppliedWallTime = currentReplStatus.optimes.lastAppliedWallTime;
        let currDurableWallTime = currentReplStatus.optimes.lastDurableWallTime;
        if (timestampCompare(prevOptime, currOptime) != 0 ||
            wallTimeCompare(prevAppliedWallTime, currAppliedWallTime) != 0 ||
            (jsTest.options().storageEngine !== "inMemory") &&
                wallTimeCompare(prevDurableWallTime, currDurableWallTime) != 0) {
            jsTest.log("optimesAndWallTimesAreEqual returning false match, prevOptime: " +
                       prevOptime + " latestOptime: " + currOptime + " prevAppliedWallTime: " +
                       prevAppliedWallTime + " latestWallTime: " + currAppliedWallTime +
                       " prevDurableWallTime: " + prevDurableWallTime + " latestDurableWallTime: " +
                       currDurableWallTime);
            replTest.dumpOplog(replTest.nodes[i], {}, 20);
            replTest.dumpOplog(replTest.nodes[i - 1], {}, 20);
            return false;
        }
        prevReplStatus = currentReplStatus;
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
assert(optimesAndWallTimesAreEqual(replTest));
var initialInfo = master.getDB('admin').serverStatus({oplog: true}).oplog;
let initialReplStatusInfo = master.getDB('admin').runCommand({replSetGetStatus: 1});

// Do an insert to increment optime, but without rolling the oplog
// latestOptime should be updated, but earliestOptime should be unchanged
var options = {writeConcern: {w: replTest.nodes.length}};
assert.writeOK(master.getDB('test').foo.insert({a: 1}, options));
assert(optimesAndWallTimesAreEqual(replTest));

var info = master.getDB('admin').serverStatus({oplog: true}).oplog;
let replStatusInfo = master.getDB('admin').runCommand({replSetGetStatus: 1});
assert.gt(timestampCompare(info.latestOptime, initialInfo.latestOptime), 0);
assert.gt(wallTimeCompare(replStatusInfo.optimes.lastAppliedWallTime,
                          initialReplStatusInfo.optimes.lastAppliedWallTime),
          0);
if (jsTest.options().storageEngine !== "inMemory") {
    assert.gt(wallTimeCompare(replStatusInfo.optimes.lastDurableWallTime,
                              initialReplStatusInfo.optimes.lastDurableWallTime),
              0);
}
assert.eq(timestampCompare(info.earliestOptime, initialInfo.earliestOptime), 0);

// Insert some large documents to force the oplog to roll over
var largeString = new Array(1024 * 10).toString();
for (var i = 0; i < 2000; i++) {
    master.getDB('test').foo.insert({largeString: largeString}, options);
}
assert.soon(function() {
    return optimesAndWallTimesAreEqual(replTest);
});

// This block requires a fresh stable checkpoint.
assert.soon(function() {
    // Test that earliestOptime was updated
    info = master.getDB('admin').serverStatus({oplog: true}).oplog;
    replStatusInfo = master.getDB('admin').runCommand({replSetGetStatus: 1});
    return timestampCompare(info.latestOptime, initialInfo.latestOptime) > 0 &&
        wallTimeCompare(replStatusInfo.optimes.lastAppliedWallTime,
                        initialReplStatusInfo.optimes.lastAppliedWallTime) > 0 &&
        ((jsTest.options().storageEngine == "inMemory") ||
         wallTimeCompare(replStatusInfo.optimes.lastDurableWallTime,
                         initialReplStatusInfo.optimes.lastDurableWallTime) > 0) &&
        timestampCompare(info.earliestOptime, initialInfo.earliestOptime) > 0;
});

replTest.stopSet();
