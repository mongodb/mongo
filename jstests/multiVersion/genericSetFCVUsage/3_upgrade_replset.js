import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isFinished, startParallelOps} from "jstests/libs/test_background_ops.js";
import {reconnect} from "jstests/replsets/rslib.js";

for (let oldVersion of ["last-lts", "last-continuous"]) {
    jsTest.log("Testing upgrade/downgrade with " + oldVersion);

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

    jsTest.log("Upgrading replica set from " + oldVersion + " to latest");

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

    jsTest.log("Downgrading replica set from latest to " + oldVersion);
    rst.upgradeSet({binVersion: oldVersion});
    jsTest.log("Replica set downgraded.");

    // Allow even more valid writes to go through
    sleep(10 * 1000);

    // Since the primary from before the upgrade took place was restarted as part of the
    // upgrade/downgrade process, we explicitly reconnect to it so that sending it an update
    // operation silently fails with an unchecked NotWritablePrimary error rather than a network
    // error.
    reconnect(oldPrimary.getDB("admin"));
    joinFindInsert();

    // Since the primary from after the upgrade took place was restarted as part of the downgrade
    // process, we explicitly reconnect to it.
    reconnect(primary.getDB("admin"));
    let totalInserts = primary.getCollection(insertNS).find().sort({_id: -1}).next()._id + 1;
    let dataFound = primary.getCollection(insertNS).count();

    jsTest.log("Found " + dataFound + " docs out of " + tojson(totalInserts) + " inserted.");

    assert.gt(dataFound / totalInserts, 0.5);

    rst.stopSet();
}
