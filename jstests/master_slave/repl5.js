// Test auto reclone after failed initial clone

soonCountAtLeast = function(db, coll, count) {
    assert.soon(function() {
        try {
            //                print( "count: " + s.getDB( db )[ coll ].find().count() );
            return s.getDB(db)[coll].find().itcount() >= count;
        } catch (e) {
            return false;
        }
    });
};

doTest = function(signal, extraOpts) {

    rt = new ReplTest("repl5tests");

    m = rt.start(true);

    // Use a database that lexicographically follows "admin" to avoid failing to clone admin, since
    // as of SERVER-29452 mongod fails to start up without a featureCompatibilityVersion document.
    ma = m.getDB("b").a;
    var bulk = ma.initializeUnorderedBulkOp();
    for (i = 0; i < 100000; ++i)
        bulk.insert({i: i});
    assert.writeOK(bulk.execute());

    s = rt.start(false, extraOpts);
    soonCountAtLeast("b", "a", 1);
    rt.stop(false, signal);

    s = rt.start(false, extraOpts, true);
    soonCountAtLeast("b", "a", 10000);

    rt.stop();
};

doTest(15);  // SIGTERM
