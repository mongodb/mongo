// Tests that the replica set's term increases once per election, and persists across a restart of
// the entire set.
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any replica set configuration document after a restart,
// so cannot elect a primary. This test induces such a scenario, so cannot be run on ephemeral
// storage engines.
// @tags: [requires_persistence]
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getLatestOp} from "jstests/replsets/rslib.js";

function getCurrentTerm(primary) {
    let res = primary.adminCommand({replSetGetStatus: 1});
    assert.commandWorked(res);
    return res.term;
}

let name = "restore_term";
let rst = new ReplSetTest({name: name, nodes: 2});

rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

let primary = rst.getPrimary();
let primaryColl = primary.getDB("test").coll;

// Current term may be greater than 1 if election race happens.
let firstSuccessfulTerm = getCurrentTerm(primary);
assert.gte(firstSuccessfulTerm, 1);
assert.commandWorked(primaryColl.insert({x: 1}, {writeConcern: {w: "majority"}}));
assert.eq(getCurrentTerm(primary), firstSuccessfulTerm);

// Check that the insert op has the initial term.
let latestOp = getLatestOp(primary);
assert.eq(latestOp.op, "i");
assert.eq(latestOp.t, firstSuccessfulTerm);

// Step down to increase the term.
assert.commandWorked(primary.adminCommand({replSetStepDown: 0}));

rst.awaitSecondaryNodes();
// The secondary became the new primary now with a higher term.
// Since there's only one secondary who may run for election, the new term is higher by 1.
assert.eq(getCurrentTerm(rst.getPrimary()), firstSuccessfulTerm + 1);

// Restart the replset and verify the term is the same.
rst.stopSet(null /* signal */, true /* forRestart */);
rst.startSet({restart: true});
rst.awaitSecondaryNodes();
primary = rst.getPrimary();

assert.eq(primary.getDB("test").coll.find().itcount(), 1);
// After restart, the new primary stands up with the newer term.
assert.gte(getCurrentTerm(primary), firstSuccessfulTerm + 1);

rst.stopSet();
