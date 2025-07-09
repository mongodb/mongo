// Test that you can still authenticate a replset connection to a RS with no primary (SERVER-6665).
(function() {
'use strict';

const NODE_COUNT = 2;
const rs = new ReplSetTest({
    "nodes": [
        {},
        // Set priority: 0 to ensure that this node can't become the primary.
        {rsConfig: {priority: 0}},
    ],
    keyFile: "jstests/libs/key1"
});
const nodes = rs.startSet();
rs.initiate();

// Add user
const primary = rs.getPrimary();
primary.getDB("admin").createUser({user: "admin", pwd: "pwd", roles: ["root"]}, {w: NODE_COUNT});

// Can authenticate replset connection when whole set is up.
const conn = new Mongo(rs.getURL());
assert(conn.getDB('admin').auth('admin', 'pwd'));
assert.commandWorked(conn.getDB('admin').foo.insert({a: 1}, {writeConcern: {w: NODE_COUNT}}));

// Make sure there is no primary
rs.stop(0);
rs.waitForState(nodes[1], ReplSetTest.State.SECONDARY);

// Make sure you can still authenticate a replset connection with no primary
const conn2 = new Mongo(rs.getURL());
conn2.setSecondaryOk();
assert(conn2.getDB('admin').auth({user: 'admin', pwd: 'pwd', mechanism: "SCRAM-SHA-1"}));
assert.eq(1, conn2.getDB('admin').foo.findOne().a);

rs.stopSet();
}());
