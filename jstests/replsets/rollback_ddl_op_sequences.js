/**
 * Basic test of a succesful replica set rollback for DDL operations.
 *
 * This tests sets up a 3 node set, data-bearing nodes A and B and an arbiter.
 *
 * 1. A is elected PRIMARY and receives several writes, which are propagated to B.
 * 2. A is isolated from the rest of the set and B is elected PRIMARY.
 * 3. B receives several operations, which will later be undone during rollback.
 * 4. B is then isolated and A regains its connection to the arbiter.
 * 5. A receives many new operations, which B will replicate after rollback.
 * 6. B rejoins the set and goes through the rollback process.
 * 7. The contents of A and B are compare to ensure the rollback results in consistent nodes.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {awaitOpTime} from "jstests/replsets/rslib.js";

// helper function for verifying contents at the end of the test
let checkFinalResults = function (db) {
    assert.eq(2, db.b.getIndexes().length);
    assert.eq(2, db.oldname.getIndexes().length);
    assert.eq(2, db.oldname.find().itcount());
    assert.eq(1, db.kap.find().itcount());
    assert(db.kap.isCapped());
    assert.eq(0, db.bar.count({q: 70}));
    assert.eq(33, db.bar.findOne({q: 0})["y"]);
    assert.eq(0, db.bar.count({q: 70}));
    assert.eq(1, db.bar.count({txt: "foo"}));
    assert.eq(200, db.bar.count({i: {$gt: -1}}));
    assert.eq(6, db.bar.count({q: {$gt: -1}}));
    assert.eq(0, db.getSiblingDB("abc").foo.find().itcount());
    assert.eq(0, db.getSiblingDB("abc").bar.find().itcount());
};

let name = "rollback_ddl_op_sequences";
let replTest = new ReplSetTest({
    name: name,
    nodes: 3,
    useBridge: true,
});
let nodes = replTest.nodeList();

let conns = replTest.startSet();
replTest.initiate(
    {
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0], priority: 3},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], arbiterOnly: true},
        ],
    },
    null,
    {initiateWithDefaultElectionTimeout: true},
);

// Make sure we have a primary and that that primary is node A
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
let primary = replTest.getPrimary();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
replTest.awaitReplication();

let a_conn = conns[0];
a_conn.setSecondaryOk();
let A = a_conn.getDB("admin");
let b_conn = conns[1];
b_conn.setSecondaryOk();
let B = b_conn.getDB("admin");
assert.eq(primary, conns[0], "conns[0] assumed to be primary");
assert.eq(a_conn, primary);

// Wait for initial replication
let a = a_conn.getDB("foo");
let b = b_conn.getDB("foo");

// This test create indexes with fail point enabled on secondary which prevents secondary from
// voting. So, disabling index build commit quorum.
// initial data for both nodes
assert.commandWorked(a.b.insert({x: 1}));
assert.commandWorked(a.b.createIndex({x: 1}, {}, 0));
assert.commandWorked(a.oldname.insert({y: 1}));
assert.commandWorked(a.oldname.insert({y: 2}));
assert.commandWorked(a.oldname.createIndex({y: 1}, {unique: true}, 0));
assert.commandWorked(a.bar.insert({q: 0}));
assert.commandWorked(a.bar.insert({q: 1, a: "foo"}));
assert.commandWorked(a.bar.insert({q: 2, a: "foo", x: 1}));
assert.commandWorked(a.bar.insert({q: 3, bb: 9, a: "foo"}));
assert.commandWorked(a.bar.insert({q: 40333333, a: 1}));
for (let i = 0; i < 200; i++) {
    assert.commandWorked(a.bar.insert({i: i}));
}
assert.commandWorked(a.bar.insert({q: 40, a: 2}));
assert.commandWorked(a.bar.insert({q: 70, txt: "willremove"}));
a.createCollection("kap", {capped: true, size: 5000});
assert.commandWorked(a.kap.insert({foo: 1}));
replTest.awaitReplication();

// isolate A and wait for B to become primary
conns[0].disconnect(conns[1]);
conns[0].disconnect(conns[2]);
assert.soon(function () {
    try {
        return B.hello().isWritablePrimary;
    } catch (e) {
        return false;
    }
});

// do operations on B and B alone, these will be rolled back
assert.commandWorked(b.bar.insert({q: 4}));
assert.commandWorked(b.bar.update({q: 3}, {q: 3, rb: true}));
assert.commandWorked(b.bar.remove({q: 40})); // multi remove test
assert.commandWorked(b.bar.update({q: 2}, {q: 39, rb: true}));
// rolling back a delete will involve reinserting the item(s)
assert.commandWorked(b.bar.remove({q: 1}));
assert.commandWorked(b.bar.update({q: 0}, {$inc: {y: 1}}));
assert.commandWorked(b.kap.insert({foo: 2}));
assert.commandWorked(b.kap2.insert({foo: 2}));
// create a collection (need to roll back the whole thing)
assert.commandWorked(b.newcoll.insert({a: true}));
// create a new empty collection (need to roll back the whole thing)
b.createCollection("abc");
// drop a collection - we'll need all its data back!
b.bar.drop();
// drop an index - verify it comes back
b.b.dropIndexes();
// two to see if we transitively rollback?
b.oldname.renameCollection("newname");
b.newname.renameCollection("fooname");
assert(b.fooname.find().itcount() > 0, "count rename");
// create an index - verify that it is removed
assert.commandWorked(b.fooname.createIndex({q: 1}, {}, 0));
// test roll back (drop) a whole database
let abc = b.getSiblingDB("abc");
assert.commandWorked(abc.foo.insert({x: 1}));
assert.commandWorked(abc.bar.insert({y: 999}));

// isolate B, bring A back into contact with the arbiter, then wait for A to become primary
// insert new data into A so that B will need to rollback when it reconnects to A
conns[1].disconnect(conns[2]);
assert.soon(function () {
    try {
        return !B.hello().isWritablePrimary;
    } catch (e) {
        return false;
    }
});

conns[0].reconnect(conns[2]);
assert.soon(function () {
    try {
        return A.hello().isWritablePrimary;
    } catch (e) {
        return false;
    }
});
assert(a.bar.find().itcount() >= 1, "count check");
assert.commandWorked(a.bar.insert({txt: "foo"}));
assert.commandWorked(a.bar.remove({q: 70}));
assert.commandWorked(a.bar.update({q: 0}, {$inc: {y: 33}}));

// A is 1 2 3 7 8
// B is 1 2 3 4 5 6
// put B back in contact with A and arbiter, as A is primary, B will rollback and then catch up
conns[1].reconnect(conns[2]);
conns[0].reconnect(conns[1]);

awaitOpTime(b_conn, a_conn);

// await steady state and ensure the two nodes have the same contents
replTest.awaitSecondaryNodes();
replTest.awaitReplication();
checkFinalResults(a);
checkFinalResults(b);

// Verify data consistency between nodes.
replTest.checkReplicatedDataHashes();
replTest.checkOplogs();

replTest.stopSet(15);
