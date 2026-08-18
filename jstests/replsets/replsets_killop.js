// Test correctness of replication while a secondary's get more requests are killed on the primary
// using killop.  SERVER-7952

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let numDocs = 1e5;

// Set up a replica set.
let replTest = new ReplSetTest({name: "test", nodes: 3});
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();
let testDb = primary.getDB("test");
testDb.test.save({a: 0});
replTest.awaitReplication();
assert.soon(function () {
    try {
        return secondary.getDB("test").test.find().itcount() == 1;
    } catch (e) {
        // The server may transiently report NotEnoughHealthyLogServersFound when
        // log servers are still warming up or recovering from killed getmores.
        return false;
    }
});

// Start a parallel shell to insert new documents on the primary.
let inserter = startParallelShell(
    funWithArgs(async function (numDocs) {
        const {runWithRetries} = await import("jstests/libs/run_with_retries.js");
        // Retry the bulk execute on transient errors (e.g. NotEnoughHealthyLogServersFound)
        // that can occur when replication getmores are being killed concurrently. Rebuild the
        // bulk on each retry because the bulk object is not safely reusable after execute()
        // throws. Fixed 1s backoff keeps total delay bounded.
        runWithRetries(
            () => {
                let bulk = db.test.initializeUnorderedBulkOp();
                for (let i = 1; i < numDocs; ++i) {
                    bulk.insert({a: i});
                }
                bulk.execute();
            },
            (e) => e.message && e.message.includes("NotEnoughHealthyLogServersFound"),
            6,
            1000,
        );
    }, numDocs),
    primary.port,
);

// Periodically kill replication get mores.
for (let i = 0; i < 1e3; ++i) {
    try {
        let allOps = testDb.currentOp();
        for (let j in allOps.inprog) {
            let op = allOps.inprog[j];
            if (op.ns == "local.oplog.rs" && op.op == "getmore") {
                testDb.killOp(op.opid);
            }
        }
    } catch (e) {
        // Only ignore transient errors from killed getmores; rethrow anything else
        // so the test doesn't silently skip killing getmores on persistent failures.
        if (!(e.message && e.message.includes("NotEnoughHealthyLogServersFound"))) {
            throw e;
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
    let count;
    try {
        count = secondary.getDB("test").test.find().itcount();
    } catch (e) {
        // Transient errors while log servers recover from killed getmores.
        print("allReplicated: transient error counting docs: " + e);
        return false;
    }
    if (count == numDocs) {
        // Return true if the count is as expected.
        return true;
    }

    // Identify and print the missing a-values.
    let foundSet = {};
    try {
        let c = secondary.getDB("test").test.find();
        while (c.hasNext()) {
            foundSet["" + c.next().a] = true;
        }
    } catch (e) {
        // Transient errors while log servers recover from killed getmores.
        print("allReplicated: transient error scanning docs: " + e);
        return false;
    }
    let missing = [];
    for (let i = 0; i < numDocs; ++i) {
        if (!("" + i in foundSet)) {
            missing.push(i);
        }
    }
    print("count: " + count + " missing: " + missing);
    return false;
}

// Wait for the correct number of (replicated) documents to be present on the secondary.
assert.soon(allReplicated, "didn't replicate all docs", 5 * 60 * 1000);
replTest.stopSet();
