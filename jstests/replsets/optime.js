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

function optimesAndWallTimesAreEqual(replTest, isPersistent) {
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
            (isPersistent && wallTimeCompare(prevDurableWallTime, currDurableWallTime) != 0)) {
            jsTest.log("optimesAndWallTimesAreEqual returning false match, prevOptime: " +
                       tojson(prevOptime) + " latestOptime: " + tojson(currOptime) +
                       " prevAppliedWallTime: " + tojson(prevAppliedWallTime) +
                       " latestWallTime: " + tojson(currAppliedWallTime) +
                       " prevDurableWallTime: " + tojson(prevDurableWallTime) +
                       " latestDurableWallTime: " + tojson(currDurableWallTime));
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

const nodes = replTest.startSet();

// Tests that serverStatus oplog returns null timestamps if the oplog collection doesn't exist.
const zeroTs = new Timestamp(0, 0);
const oplogStatus = nodes[0].getDB('admin').serverStatus({oplog: true}).oplog;
assert.eq(oplogStatus.earliestOptime, zeroTs);
assert.eq(oplogStatus.latestOptime, zeroTs);

replTest.initiate();
var primary = replTest.getPrimary();
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

const isPersistent = primary.getDB('admin').serverStatus().storageEngine.persistent;

// Check initial optimes
assert.soon(function() {
    return optimesAndWallTimesAreEqual(replTest, isPersistent);
});
var initialInfo = primary.getDB('admin').serverStatus({oplog: true}).oplog;
let initialReplStatusInfo = primary.getDB('admin').runCommand({replSetGetStatus: 1});

// Do an insert to increment optime, but without rolling the oplog
// latestOptime should be updated, but earliestOptime should be unchanged
var options = {writeConcern: {w: replTest.nodes.length}};
if (isPersistent) {
    // Ensure the durable optime is advanced.
    options.writeConcern.j = true;
}
assert.commandWorked(primary.getDB('test').foo.insert({a: 1}, options));
assert.soon(function() {
    return optimesAndWallTimesAreEqual(replTest, isPersistent);
});

var info = primary.getDB('admin').serverStatus({oplog: true}).oplog;
var entry = primary.getDB('local').oplog.rs.findOne().ts;
jsTest.log("First entry's timestamp is " + tojson(entry));
let replStatusInfo = primary.getDB('admin').runCommand({replSetGetStatus: 1});

const dumpInfoFn = function() {
    jsTestLog("Initial server status: " + tojsononeline(initialInfo));
    jsTestLog("Initial replSetGetStatus: " + tojsononeline(initialReplStatusInfo));
    jsTestLog("Final server status: " + tojsononeline(info));
    jsTestLog("Final replSetGetStatus: " + tojsononeline(replStatusInfo));
};

assert.gt(timestampCompare(info.latestOptime, initialInfo.latestOptime), 0, dumpInfoFn);
assert.eq(timestampCompare(info.earliestOptime, initialInfo.earliestOptime), 0, dumpInfoFn);

// Insert some large documents to force the oplog to roll over
var largeString = new Array(1024 * 10).toString();
for (var i = 0; i < 2000; i++) {
    primary.getDB('test').foo.insert({largeString: largeString}, options);
}
assert.soon(function() {
    return optimesAndWallTimesAreEqual(replTest, isPersistent);
});
entry = primary.getDB('local').oplog.rs.findOne().ts;
jsTest.log("First entry's timestamp is now " + tojson(entry) + " after oplog rollover");

// This block requires a fresh stable checkpoint.
assert.soon(function() {
    // Test that earliestOptime was updated
    info = primary.getDB('admin').serverStatus({oplog: true}).oplog;
    jsTest.log("Earliest optime is now " + tojson(info.earliestOptime) +
               "; looking for it to be different from " + tojson(initialInfo.earliestOptime));
    replStatusInfo = primary.getDB('admin').runCommand({replSetGetStatus: 1});
    return timestampCompare(info.latestOptime, initialInfo.latestOptime) > 0 &&
        timestampCompare(info.earliestOptime, initialInfo.earliestOptime) > 0;
});

replTest.stopSet();
