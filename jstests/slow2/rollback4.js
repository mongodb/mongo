// Test a large rollback SERVER-2737
//
// This test is disabled on ephemeral storage engines, because it stops one of the data bearing
// nodes and as such the data would be lost.
// @tags: [requires_persistence]
(function() {
'use strict';

var replTest = new ReplSetTest({ name: 'unicomplex', 
                                 nodes: 3, 
                                 oplogSize: 2000
                              });
var nodes = replTest.nodeList();

var conns = replTest.startSet();
var r = replTest.initiate({ "_id": "unicomplex",
                            "settings": {
                                "heartbeatTimeoutSecs":30
                            },
                            "members": [
                                { "_id": 0, "host": nodes[0], priority: 2 },
                                { "_id": 1, "host": nodes[1] },
                                { "_id": 2, "host": nodes[2], arbiterOnly: true}]
                          }, 'replSetInitiate', 600000);

replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY, 60 * 1000);
// Make sure we have a master
var master = replTest.getPrimary();
var b_conn = conns[1];
b_conn.setSlaveOk();
var B = b_conn.getDB("admin");

// Make sure we have an arbiter
replTest.waitForState(conns[2], ReplSetTest.State.ARBITER, 10000);

// Wait for initial replication
replTest.awaitReplication();

// Insert into master
var big = { b:new Array( 1000 ).toString() };
var bulk = master.getDB('db').c.initializeUnorderedBulkOp();
for( var i = 0; i < 1000000; ++i ) {
    bulk.insert( big );
}
assert.writeOK(bulk.execute());

// Stop master
replTest.stop( 0 );

// Wait for slave to take over
// This can take a while if the secondary has queued up many writes in its
// buffer, since it needs to flush those out before it can assume the primaryship.
//
// In the legacy replication implementation (through 2.7.7), this waiting takes place before the
// node reports that it is primary, while in the refactored implementation (2.7.8+) it takes place
// after the node reports that it is primary via heartbeats, but before ismaster indicates that the
// node will accept writes.
replTest.waitForState(conns[1], ReplSetTest.State.PRIMARY, 5 * 60 * 1000);
master = replTest.getPrimary(5 * 60 * 1000);

// Save to new master, forcing rollback of old master
master.getDB( 'db' ).c.save( big );

// Restart old master
replTest.restart( 0 );
// Wait five minutes to ensure there is enough time for rollback
replTest.awaitSecondaryNodes(5*60*1000);
replTest.awaitReplication(5*60*1000);

})();
