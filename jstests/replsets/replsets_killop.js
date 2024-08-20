// Test correctness of replication while a secondary's get more requests are killed on the primary
// using killop.  SERVER-7952

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let numDocs = 1e5;

// Set up a replica set.
let replTest = new ReplSetTest({name: 'test', nodes: 3});
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let testDb = primary.getDB('test');
testDb.test.save({a: 0});
replTest.awaitReplication();
assert.soon(function() {
    return secondary.getDB('test').test.find().itcount() == 1;
});

// Start a parallel shell to insert new documents on the primary.
let inserter = startParallelShell(funWithArgs(function(numDocs) {
                                      var bulk = db.test.initializeUnorderedBulkOp();
                                      for (let i = 1; i < numDocs; ++i) {
                                          bulk.insert({a: i});
                                      }
                                      bulk.execute();
                                  }, numDocs), primary.port);

// Periodically kill replication get mores.
for (let i = 0; i < 1e3; ++i) {
    let allOps = testDb.currentOp();
    for (let j in allOps.inprog) {
        let op = allOps.inprog[j];
        if (op.ns == 'local.oplog.rs' && op.op == 'getmore') {
            testDb.killOp(op.opid);
        }
    }
    sleep(100);
}

// Wait for the inserter to finish.
inserter();

assert.eq(numDocs, testDb.test.find().itcount());

// Return true when the correct number of documents are present on the secondary.  Otherwise print
// which documents are missing and return false.
function allReplicated() {
    let count = secondary.getDB('test').test.find().itcount();
    if (count == numDocs) {
        // Return true if the count is as expected.
        return true;
    }

    // Identify and print the missing a-values.
    let foundSet = {};
    let c = secondary.getDB('test').test.find();
    while (c.hasNext()) {
        foundSet['' + c.next().a] = true;
    }
    let missing = [];
    for (let i = 0; i < numDocs; ++i) {
        if (!(('' + i) in foundSet)) {
            missing.push(i);
        }
    }
    print('count: ' + count + ' missing: ' + missing);
    return false;
}

// Wait for the correct number of (replicated) documents to be present on the secondary.
assert.soon(allReplicated, "didn't replicate all docs", 5 * 60 * 1000);
replTest.stopSet();
