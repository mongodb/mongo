import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isFinished, startParallelOps} from "jstests/libs/test_background_ops.js";
import {reconnect} from "jstests/replsets/rslib.js";

let oldVersion = "last-lts";

let nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion},
    a3: {binVersion: oldVersion},
};

let rst = new ReplSetTest({nodes: nodes});

rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// Wait for a primary node...
var primary = rst.getPrimary();
let otherOpConn = new Mongo(rst.getURL());
let insertNS = "test.foo";

jsTest.log("Starting parallel operations during upgrade...");

function findAndInsert(rsURL, coll) {
    var coll = new Mongo(rsURL).getCollection(coll + "");
    let count = 0;

    jsTest.log("Starting finds and inserts...");

    while (!isFinished()) {
        try {
            assert.commandWorked(coll.insert({_id: count, hello: "world"}));
            assert.neq(null, coll.findOne({_id: count}));
        } catch (e) {
            printjson(e);
        }

        count++;
    }

    jsTest.log("Finished finds and inserts...");
    return count;
}

let joinFindInsert = startParallelOps(
    primary, // The connection where the test info is passed and stored
    findAndInsert,
    [rst.getURL(), insertNS],
);

jsTest.log("Upgrading replica set...");

rst.upgradeSet({binVersion: "latest"});

jsTest.log("Replica set upgraded.");

// We save a reference to the old primary so that we can call reconnect() on it before
// joinFindInsert() would attempt to send the node an update operation that signals the parallel
// shell running the background operations to stop.
let oldPrimary = primary;

// Wait for primary
var primary = rst.getPrimary();

printjson(rst.status());

// Allow more valid writes to go through
sleep(10 * 1000);

// Since the old primary was restarted as part of the upgrade process, we explicitly reconnect to it
// so that sending it an update operation silently fails with an unchecked NotWritablePrimary error
// rather than a network error.
reconnect(oldPrimary.getDB("admin"));
joinFindInsert();

let totalInserts = primary.getCollection(insertNS).find().sort({_id: -1}).next()._id + 1;
let dataFound = primary.getCollection(insertNS).count();

jsTest.log("Found " + dataFound + " docs out of " + tojson(totalInserts) + " inserted.");

assert.gt(dataFound / totalInserts, 0.5);

rst.stopSet();
