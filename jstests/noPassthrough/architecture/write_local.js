// SERVER-22011: Deadlock in ticket distribution
// @tags: [requires_replication, requires_capped]
// Limit concurrent WiredTiger transactions to maximize locking issues, harmless for other SEs.
import {ReplSetTest} from "jstests/libs/replsettest.js";

let options = {verbose: 1};

// Create a new single node replicaSet
let replTest = new ReplSetTest({name: "write_local", nodes: 1, oplogSize: 1, nodeOptions: options});
replTest.startSet();
replTest.initiate();
let mongod = replTest.getPrimary();
mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 1});

let local = mongod.getDB("local");

// Start inserting documents in test.capped and local.capped capped collections.
let shells = ["test", "local"].map(function (dbname) {
    let mydb = local.getSiblingDB(dbname);
    mydb.capped.drop();
    mydb.createCollection("capped", {capped: true, size: 20 * 1000});
    return startParallelShell(
        'var mydb=db.getSiblingDB("' +
            dbname +
            '"); ' +
            "(function() { " +
            "    for(var i=0; i < 10*1000; i++) { " +
            "        mydb.capped.insert({ x: i }); " +
            "    } " +
            "})();",
        mongod.port,
    );
});

// The following causes inconsistent locking order in the ticket system, depending on
// timeouts to avoid deadlock.
let oldObjects = 0;
for (let i = 0; i < 1000; i++) {
    print(local.stats().objects);
    sleep(1);
}

// Wait for parallel shells to terminate and stop our replset.
shells.forEach(function (f) {
    f();
});
replTest.stopSet();
