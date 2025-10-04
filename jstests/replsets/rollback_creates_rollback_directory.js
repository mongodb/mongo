// test that a rollback directory is created during a replica set rollback
// this also tests that updates are recorded in the rollback file
//  (this test does no delete rollbacks)
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

import {ReplSetTest} from "jstests/libs/replsettest.js";

function runRollbackDirectoryTest(shouldCreateRollbackFiles) {
    jsTestLog("Testing createRollbackDataFiles = " + shouldCreateRollbackFiles);
    let testName = "rollback_creates_rollback_directory";
    let replTest = new ReplSetTest({name: testName, nodes: 3});
    let nodes = replTest.nodeList();

    let conns = replTest.startSet({setParameter: {createRollbackDataFiles: shouldCreateRollbackFiles}});
    let r = replTest.initiate(
        {
            "_id": testName,
            "members": [
                {"_id": 0, "host": nodes[0], priority: 3},
                {"_id": 1, "host": nodes[1]},
                {"_id": 2, "host": nodes[2], arbiterOnly: true},
            ],
        },
        null,
        {initiateWithDefaultElectionTimeout: true},
    );

    // Make sure we have a primary
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
    let primary = replTest.getPrimary();
    let a_conn = conns[0];
    let b_conn = conns[1];
    a_conn.setSecondaryOk();
    b_conn.setSecondaryOk();
    let A = a_conn.getDB("test");
    let B = b_conn.getDB("test");
    let Apath = replTest.getDbPath(a_conn) + "/";
    let Bpath = replTest.getDbPath(b_conn) + "/";
    assert(primary == conns[0], "conns[0] assumed to be primary");
    assert(a_conn.host == primary.host);

    // Make sure we have an arbiter
    assert.soon(function () {
        let res = conns[2].getDB("admin").runCommand({replSetGetStatus: 1});
        return res.myState == 7;
    }, "Arbiter failed to initialize.");

    let options = {writeConcern: {w: 2, wtimeout: replTest.timeoutMS}, upsert: true};
    assert.commandWorked(A.foo.update({key: "value1"}, {$set: {req: "req"}}, options));
    let AID = replTest.getNodeId(a_conn);
    replTest.stop(AID);

    primary = replTest.getPrimary();
    assert(b_conn.host == primary.host);
    options = {writeConcern: {w: 1, wtimeout: replTest.timeoutMS}, upsert: true};
    assert.commandWorked(B.foo.update({key: "value1"}, {$set: {res: "res"}}, options));
    let BID = replTest.getNodeId(b_conn);
    replTest.stop(BID);
    replTest.restart(AID);
    primary = replTest.getPrimary();
    assert(a_conn.host == primary.host);
    options = {writeConcern: {w: 1, wtimeout: replTest.timeoutMS}, upsert: true};
    assert.commandWorked(A.foo.update({key: "value2"}, {$set: {req: "req"}}, options));
    replTest.restart(BID); // should rollback
    reconnect(B);

    print("BEFORE------------------");
    printjson(A.foo.find().toArray());

    replTest.awaitReplication();
    replTest.awaitSecondaryNodes();

    print("AFTER------------------");
    printjson(A.foo.find().toArray());

    assert.eq(2, A.foo.find().itcount());
    assert.eq("req", A.foo.findOne({key: "value1"}).req);
    assert.eq(null, A.foo.findOne({key: "value1"}).res);
    reconnect(B);
    assert.eq(2, B.foo.find().itcount());
    assert.eq("req", B.foo.findOne({key: "value1"}).req);
    assert.eq(null, B.foo.findOne({key: "value1"}).res);

    // check here for rollback files
    let rollbackDir = Bpath + "rollback/";
    assert.eq(pathExists(rollbackDir), shouldCreateRollbackFiles, rollbackDir);

    // Verify data consistency between nodes.
    replTest.checkReplicatedDataHashes();
    replTest.checkOplogs();

    print(testName + ".js SUCCESS");
    replTest.stopSet(15);

    function wait(f) {
        let n = 0;
        while (!f()) {
            if (n % 4 == 0) print(testName + ".js waiting");
            if (++n == 4) {
                print("" + f);
            }
            assert(n < 200, "tried 200 times, giving up");
            sleep(1000);
        }
    }

    function reconnect(a) {
        wait(function () {
            try {
                a.bar.stats();
                return true;
            } catch (e) {
                print(e);
                return false;
            }
        });
    }
}

runRollbackDirectoryTest(false);
runRollbackDirectoryTest(true);
