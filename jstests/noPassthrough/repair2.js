// SERVER-2843 The repair command should not yield.

(function() {
    "use strict";
    const baseName = "jstests_repair2";

    const conn = MongoRunner.runMongod();
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
            bulk.find({_id: j}).remove();
        }

        // The bulk operation is expected to succeed, or fail due to interrupt depending on the
        // storage engine.
        try {
            bulk.execute();
            assert.eq(0, t.count());
        } catch (ex) {
            ex.toResult().getWriteErrors().forEach(function(error) {
                assert.eq(error.code, ErrorCodes.QueryPlanKilled, tojson(error));
                assert.eq(error.errmsg, "database closed for repair", tojson(error));
            });
            // The failure may have been a remove, so continuing to the next iteration in the loop
            // would result in a duplicate key on insert.
            break;
        }
    }

    awaitShell();

    MongoRunner.stopMongod(conn);
})();
