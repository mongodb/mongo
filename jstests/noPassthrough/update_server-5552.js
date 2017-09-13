var db;
(function() {
    "use strict";
    const conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
    assert.neq(null, conn, "mongod failed to start.");
    db = conn.getDB("test");

    const t = db.foo;
    t.drop();

    const N = 10000;

    var bulk = t.initializeUnorderedBulkOp();
    for (let i = 0; i < N; i++) {
        bulk.insert({_id: i, x: 1});
    }
    assert.writeOK(bulk.execute());

    const join = startParallelShell(
        "while( db.foo.findOne( { _id : 0 } ).x == 1 ); db.foo.ensureIndex( { x : 1 } );");

    t.update({
        $where: function() {
            sleep(1);
            return true;
        }
    },
             {$set: {x: 5}},
             false,
             true);
    db.getLastError();

    join();

    assert.eq(N, t.find({x: 5}).count());

    MongoRunner.stopMongod(conn);
})();
