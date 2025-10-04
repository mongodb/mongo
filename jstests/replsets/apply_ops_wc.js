/**
 * apply_ops_wc.js
 *
 * This file tests SERVER-22270 that applyOps commands should take a writeConcern.
 * This first tests that invalid write concerns cause writeConcern errors.
 * Next, it tests replication with writeConcerns of w:2 and w:majority.
 * When there are 3 nodes up in a replica set, applyOps commands succeed.
 * It then stops replication at one seconday and confirms that applyOps commands still succeed.
 * It finally stops replication at another secondary and confirms that applyOps commands fail.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartReplicationOnSecondaries, stopServerReplication} from "jstests/libs/write_concern_util.js";

let nodeCount = 3;
let replTest = new ReplSetTest({name: "applyOpsWCSet", nodes: nodeCount});
replTest.startSet();
let cfg = replTest.getReplSetConfig();
cfg.settings = {};
cfg.settings.chainingAllowed = false;
replTest.initiate(cfg);

let testDB = "applyOps-wc-test";

// Get test collection.
let primary = replTest.getPrimary();

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replTest.awaitReplication();

var db = primary.getDB(testDB);
let coll = db.apply_ops_wc;

function dropTestCollection() {
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");
}

dropTestCollection();

// Set up the applyOps command.
let applyOpsReq = {
    applyOps: [
        {op: "i", ns: coll.getFullName(), o: {_id: 2, x: "b"}},
        {op: "i", ns: coll.getFullName(), o: {_id: 3, x: "c"}},
        {op: "i", ns: coll.getFullName(), o: {_id: 4, x: "d"}},
    ],
};

function assertApplyOpsCommandWorked(res) {
    assert.eq(3, res.applied);
    assert.commandWorkedIgnoringWriteConcernErrors(res);
    assert.eq([true, true, true], res.results);
}

function assertWriteConcernError(res) {
    assert(res.writeConcernError);
    assert(res.writeConcernError.code);
    assert(res.writeConcernError.errmsg);
}

let invalidWriteConcerns = [{w: "invalid"}, {w: nodeCount + 1}];

function testInvalidWriteConcern(wc) {
    jsTest.log("Testing invalid write concern " + tojson(wc));

    applyOpsReq.writeConcern = wc;
    dropTestCollection();
    assert.commandWorked(coll.insert({_id: 1, x: "a"}));
    let res = coll.runCommand(applyOpsReq);
    assertApplyOpsCommandWorked(res);
    assertWriteConcernError(res);
}

// Verify that invalid write concerns yield an error.
coll.insert({_id: 1, x: "a"});
invalidWriteConcerns.forEach(testInvalidWriteConcern);

let secondaries = replTest.getSecondaries();

let majorityWriteConcerns = [
    {w: 2, wtimeout: 30000},
    {w: "majority", wtimeout: 30000},
];

function testMajorityWriteConcerns(wc) {
    jsTest.log("Testing " + tojson(wc));

    // Reset secondaries to ensure they can replicate.
    restartReplicationOnSecondaries(replTest);

    // Set the writeConcern of the applyOps command.
    applyOpsReq.writeConcern = wc;

    dropTestCollection();

    // applyOps with a full replica set should succeed.
    assert.commandWorked(coll.insert({_id: 1, x: "a"}));
    let res = db.runCommand(applyOpsReq);

    assertApplyOpsCommandWorked(res);
    assert(
        !res.writeConcernError,
        "applyOps on a full replicaset had writeConcern error " + tojson(res.writeConcernError),
    );

    dropTestCollection();

    // Stop replication at one secondary.
    stopServerReplication(secondaries[0]);

    // applyOps should succeed with only 1 node not replicating.
    assert.commandWorked(coll.insert({_id: 1, x: "a"}));
    res = db.runCommand(applyOpsReq);

    assertApplyOpsCommandWorked(res);
    assert(
        !res.writeConcernError,
        "applyOps on a replicaset with 2 working nodes had writeConcern error " + tojson(res.writeConcernError),
    );

    dropTestCollection();

    // Stop replication at a second secondary.
    stopServerReplication(secondaries[1]);

    // applyOps should fail after two nodes have stopped replicating.
    assert.commandWorked(coll.insert({_id: 1, x: "a"}));
    applyOpsReq.writeConcern.wtimeout = 5000;
    res = db.runCommand(applyOpsReq);

    assertApplyOpsCommandWorked(res);
    assertWriteConcernError(res);
}

majorityWriteConcerns.forEach(testMajorityWriteConcerns);

// Allow clean shutdown
restartReplicationOnSecondaries(replTest);

replTest.stopSet();
