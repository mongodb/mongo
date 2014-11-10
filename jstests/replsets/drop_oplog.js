// Test that dropping either the replset oplog or the local database is prohibited in a replset.

(function () {
    "use strict";
    var rt = new ReplSetTest( { name : "drop_oplog" , nodes: 1, oplogSize: 30 } );

    var nodes = rt.startSet();
    rt.initiate();
    var master = rt.getMaster();
    var ml = master.getDB( 'local' );

    var threw = false;
    try {
        ml.oplog.rs.drop();
    }
    catch (err) {
        assert.eq(err, 
                  "Error: drop failed: { \"ok\" : 0, \"errmsg\" : " +
                  "\"can't drop live oplog while replicating\" }");
        threw = true;
    }
    assert(threw);
    var dropOutput = ml.dropDatabase();
    assert.eq(dropOutput.ok, 0);
    assert.eq(dropOutput.errmsg, "Cannot drop 'local' database while replication is active");

    var renameOutput = ml.oplog.rs.renameCollection("poison");
    assert.eq(renameOutput.ok, 0);
    assert.eq(renameOutput.errmsg, 
              "can't rename live oplog while replicating");

    assert.writeOK(ml.foo.insert( {a:1} ));
    renameOutput = ml.foo.renameCollection("oplog.rs");
    assert.eq(renameOutput.ok, 0);
    assert.eq(renameOutput.errmsg, 
              "can't rename to live oplog while replicating");

}());
