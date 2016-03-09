// SERVER-2843 The repair command should not yield.

baseName = "jstests_repair2";

load("jstests/libs/slow_weekly_util.js");
testServer = new SlowWeeklyMongod(baseName);

t = testServer.getDB(baseName)[baseName];
t.drop();

var awaitShell = startParallelShell("db = db.getSiblingDB( '" + baseName + "');" +
                                        "for( i = 0; i < 10; ++i ) { " + "db.repairDatabase();" +
                                        "sleep( 5000 );" + " }",
                                    testServer.port);

for (i = 0; i < 30; ++i) {
    var bulk = t.initializeOrderedBulkOp();
    for (j = 0; j < 5000; ++j) {
        bulk.insert({_id: j});
    }

    for (j = 0; j < 5000; ++j) {
        bulk.find({_id: j, $isolated: 1}).remove();
    }

    assert.writeOK(bulk.execute());
    assert.eq(0, t.count());
}

awaitShell();

testServer.stop();
