/**
 * Test basic read committed functionality, including:
 *  - Writes with writeConcern 'majority' should be visible once the write completes.
 *  - With the only data-bearing secondary down, committed reads should not include newly inserted
 *    data.
 *  - When data-bearing node comes back up and catches up, writes should be readable.
 *
 * @tags: [requires_majority_read_concern]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const majorityWriteConcern = {
    writeConcern: {w: "majority", wtimeout: 60 * 1000},
};

// Each test case includes a 'prepareCollection' method that sets up the initial state starting
// with an empty collection, a 'write' method that does some write, and two arrays,
// 'expectedBefore' and 'expectedAfter' that describe the expected contents of the collection
// before and after the write. The 'prepareCollection' and 'write' methods should leave the
// collection either empty or with a single document with _id: 1.
const testCases = {
    insert: {
        prepareCollection: function (coll) {}, // No-op
        write: function (coll, writeConcern) {
            assert.commandWorked(coll.insert({_id: 1}, writeConcern));
        },
        expectedBefore: [],
        expectedAfter: [{_id: 1}],
    },
    update: {
        prepareCollection: function (coll) {
            assert.commandWorked(coll.insert({_id: 1, state: "before"}, majorityWriteConcern));
        },
        write: function (coll, writeConcern) {
            assert.commandWorked(coll.update({_id: 1}, {$set: {state: "after"}}, writeConcern));
        },
        expectedBefore: [{_id: 1, state: "before"}],
        expectedAfter: [{_id: 1, state: "after"}],
    },
    remove: {
        prepareCollection: function (coll) {
            assert.commandWorked(coll.insert({_id: 1}, majorityWriteConcern));
        },
        write: function (coll, writeConcern) {
            assert.commandWorked(coll.remove({_id: 1}, writeConcern));
        },
        expectedBefore: [{_id: 1}],
        expectedAfter: [],
    },
};

// Set up a set and grab things for later.
let name = "read_committed";
let replTest = new ReplSetTest({name: name, nodes: 3});

replTest.startSet();
let nodes = replTest.nodeList();
let config = {
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1], priority: 0},
        {"_id": 2, "host": nodes[2], arbiterOnly: true},
    ],
};

replTest.initiate(config);

// Get connections and collection.
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let coll = primary.getDB(name)[name];
let secondaryColl = secondary.getDB(name)[name];

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replTest.awaitReplication();
function log(arg) {
    jsTest.log(tojson(arg));
}

function doRead(coll, readConcern) {
    readConcern.maxTimeMS = 3000;
    let res = assert.commandWorked(coll.runCommand("find", readConcern));
    return new DBCommandCursor(coll.getDB(), res).toArray();
}

function doDirtyRead(coll) {
    log("doing dirty read");
    let ret = doRead(coll, {"readConcern": {"level": "local"}});
    log("done doing dirty read.");
    return ret;
}

function doCommittedRead(coll) {
    log("doing committed read");
    let ret = doRead(coll, {"readConcern": {"level": "majority"}});
    log("done doing committed read.");
    return ret;
}

function readLatestOplogEntry(readConcernLevel) {
    let oplog = primary.getDB("local").oplog.rs;
    let res = oplog.runCommand("find", {
        "readConcern": {"level": readConcernLevel},
        "maxTimeMS": 3000,
        sort: {$natural: -1},
        limit: 1,
    });
    assert.commandWorked(res);
    return new DBCommandCursor(coll.getDB(), res).toArray()[0];
}

for (let testName in testCases) {
    jsTestLog("Running test " + testName);
    var test = testCases[testName];

    const setUpInitialState = function setUpInitialState() {
        assert.commandWorked(coll.remove({}, majorityWriteConcern));
        test.prepareCollection(coll);
        // Do some sanity checks.
        assert.eq(doDirtyRead(coll), test.expectedBefore);
        assert.eq(doCommittedRead(coll), test.expectedBefore);
    };

    // Writes done with majority write concern must be immediately visible to both dirty and
    // committed reads.
    setUpInitialState();
    test.write(coll, majorityWriteConcern);
    assert.eq(doDirtyRead(coll), test.expectedAfter);
    assert.eq(doCommittedRead(coll), test.expectedAfter);

    // Return to the initial state, then stop the secondary from applying new writes to prevent
    // them from becoming committed.
    setUpInitialState();
    stopServerReplication(secondary);
    const initialOplogTs = readLatestOplogEntry("local").ts;

    // Writes done without majority write concern must be immediately visible to dirty read
    // and hidden from committed reads until they have been replicated. The rules for seeing
    // an oplog entry for a write are the same as for the write itself.
    test.write(coll, {});
    assert.eq(doDirtyRead(coll), test.expectedAfter);
    assert.neq(readLatestOplogEntry("local").ts, initialOplogTs);
    assert.eq(doCommittedRead(coll), test.expectedBefore);
    assert.eq(readLatestOplogEntry("majority").ts, initialOplogTs);

    // Try the committed read again after sleeping to ensure it doesn't only work for
    // queries immediately after the write.
    sleep(1000);
    assert.eq(doCommittedRead(coll), test.expectedBefore);
    assert.eq(readLatestOplogEntry("majority").ts, initialOplogTs);

    // Restart oplog application on the secondary and ensure the committed view is updated.
    restartServerReplication(secondary);
    replTest.awaitLastOpCommitted();

    assert.eq(doCommittedRead(coll), test.expectedAfter);
    assert.neq(readLatestOplogEntry("majority").ts, initialOplogTs);

    // The secondary will be able to make the write committed soon after the primary, but there
    // is no way to block until it does.
    try {
        assert.soon(function () {
            return friendlyEqual(doCommittedRead(secondaryColl), test.expectedAfter);
        });
    } catch (e) {
        // generate useful error messages on failures.
        assert.eq(doCommittedRead(secondaryColl), test.expectedAfter);
    }
}
replTest.stopSet();
