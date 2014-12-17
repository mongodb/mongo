// test that the resync command works with replica sets and that one does not need to manually
// force a replica set resync by deleting all datafiles

// TODO: Remove once test is fixed for SERVER-15704, add "use strict"
if (false) {
    var replTest = new ReplSetTest({name: 'resync', nodes: 3, oplogSize: 1});
    var nodes = replTest.nodeList();

    var conns = replTest.startSet();
    var r = replTest.initiate({ "_id": "resync",
                                "members": [
                                    {"_id": 0, "host": nodes[0]},
                                    {"_id": 1, "host": nodes[1]},
                                    {"_id": 2, "host": nodes[2]}]
                              });

    // Make sure we have a master
    var master = replTest.getMaster();
    var a_conn = conns[0];
    var b_conn = conns[1];
    a_conn.setSlaveOk();
    b_conn.setSlaveOk();
    var A = a_conn.getDB("test");
    var B = b_conn.getDB("test");
    var AID = replTest.getNodeId(a_conn);
    var BID = replTest.getNodeId(b_conn);
    assert(master == conns[0], "conns[0] assumed to be master");
    assert(a_conn.host == master.host);

    // create an oplog entry with an insert
    assert.writeOK( A.foo.insert({ x: 1 }, { writeConcern: { w: 3, wtimeout: 60000 }}));
    replTest.stop(BID);

    function hasCycled() {
        var oplog = a_conn.getDB("local").oplog.rs;
        return oplog.find( { "o.x" : 1 } ).sort( { $natural : 1 } )._addSpecial( "$maxScan" , 10 ).itcount() == 0;
    }

    for ( var cycleNumber = 0; cycleNumber < 10; cycleNumber++ ) {
        // insert enough to cycle oplog
        var bulk = A.foo.initializeUnorderedBulkOp();
        for (i=2; i < 10000; i++) {
            bulk.insert({x:i});
        }

        // wait for secondary to also have its oplog cycle
        assert.writeOK(bulk.execute({ w: 2, wtimeout : 60000 }));

        if ( hasCycled() )
            break;
    }

    assert( hasCycled() );



    // bring node B and it will enter recovery mode because its newest oplog entry is too old
    replTest.restart(BID);
    // check that it is in recovery mode
    assert.soon(function() {
        try {
            var result = b_conn.getDB("admin").runCommand({replSetGetStatus: 1});
            return (result.members[1].stateStr === "RECOVERING");
        }
        catch ( e ) {
            print( e );
        }
    }, "node didn't enter RECOVERING state");

    // run resync and wait for it to happen
    assert.commandWorked(b_conn.getDB("admin").runCommand({resync:1}));
    replTest.awaitReplication();
    replTest.awaitSecondaryNodes();

    replTest.stopSet(15);
}
