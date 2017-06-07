// SERVER-2843 The repair command should not yield.

(function() {
    "use strict";
    const baseName = "jstests_repair2";

    const conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
    assert.neq(null, conn, "mongod failed to start.");

    const t = conn.getDB(baseName)[baseName];
    t.drop();

    var awaitShell = startParallelShell("db = db.getSiblingDB( '" + baseName + "');" +
                                            "for( i = 0; i < 10; ++i ) { " +
                                            "db.repairDatabase();" + "sleep( 5000 );" + " }",
                                        conn.port);

    for (let i = 0; i < 30; ++i) {
        var bulk = t.initializeOrderedBulkOp();
        for (let j = 0; j < 5000; ++j) {
            bulk.insert({_id: j});
        }

        for (let j = 0; j < 5000; ++j) {
            bulk.find({_id: j, $isolated: 1}).remove();
        }

        assert.writeOK(bulk.execute());
        assert.eq(0, t.count());
    }

    awaitShell();

    MongoRunner.stopMongod(conn);
})();
