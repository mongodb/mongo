import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isFinished, startParallelOps} from "jstests/libs/test_background_ops.js";
import {reconnect} from "jstests/replsets/rslib.js";

let newVersion = "latest";

let name = "replsetdowngrade";
let nodes = {
    n1: {binVersion: newVersion},
    n2: {binVersion: newVersion},
    n3: {binVersion: newVersion},
};

function runDowngradeTest(downgradeVersion) {
    jsTestLog("Running test with 'downgradeVersion': " + downgradeVersion);
    const downgradeFCV = binVersionToFCV(downgradeVersion);
    let rst = new ReplSetTest({name: name, nodes: nodes, waitForKeys: true});
    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    let primary = rst.getPrimary();
    let coll = "test.foo";

    // The default FCV is latestFCV for non-shard replica sets.
    let primaryAdminDB = rst.getPrimary().getDB("admin");
    checkFCV(primaryAdminDB, latestFCV);

    // We wait for the feature compatibility version to be set to the downgraded FCV on all nodes of
    // the replica set in order to ensure that all nodes can be successfully downgraded. This
    // effectively allows us to emulate upgrading to the latest version with existing data files and
    // then trying to downgrade the FCV.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));
    rst.awaitReplication();

    jsTest.log("Inserting documents into collection.");
    for (let i = 0; i < 10; i++) {
        primary.getCollection(coll).insert({_id: i, str: "hello world"});
    }

    function insertDocuments(rsURL, collParam) {
        let coll = new Mongo(rsURL).getCollection(collParam);
        let count = 10;
        while (!isFinished()) {
            assert.commandWorked(coll.insert({_id: count, str: "hello world"}));
            count++;
        }
    }

    jsTest.log("Starting parallel operations during downgrade..");
    let joinFindInsert = startParallelOps(primary, insertDocuments, [rst.getURL(), coll]);

    jsTest.log("Downgrading replica set..");
    rst.upgradeSet({binVersion: downgradeVersion});
    jsTest.log("Downgrade complete.");

    // We save a reference to the old primary so that we can call reconnect() on it before
    // joinFindInsert() would attempt to send the node an update operation that signals the parallel
    // shell running the background operations to stop.
    let oldPrimary = primary;

    primary = rst.getPrimary();
    printjson(rst.status());

    // Since the old primary was restarted as part of the downgrade process, we explicitly reconnect
    // to it so that sending it an update operation silently fails with an unchecked
    // NotWritablePrimary error rather than a network error.
    reconnect(oldPrimary.getDB("admin"));
    joinFindInsert();
    rst.stopSet();
}

runDowngradeTest("last-continuous");
runDowngradeTest("last-lts");
