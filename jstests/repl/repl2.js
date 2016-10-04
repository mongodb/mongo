// Test resync command:
//   1. Start master/slave deployment.
//   2. Insert a document to seed the oplog.
//   3. Assert that the resync command fails when the slave is caught up to the master.
//   4. Stop the slave.
//   5. Insert enough documents to rotate the oplog.
//   6. Restart the slave.
//   7. Assert the resync command now works on the slave.
//   8. Assert the slave eventually has the same data.
//   9. Assert the slave now rejects the resync command.
//
// This test cannot be run on ephemeral storage engines, because after restarting, at step 6, the
// slave will not have any data and will start an initial sync, rejecting the resync command.
// @tags: [requires_persistence]

soonCount = function(count) {
    assert.soon(function() {
        //                print( "check count" );
        //                print( "count: " + s.getDB( baseName ).z.find().count() );
        return s.getDB("foo").a.find().count() == count;
    });
};

doTest = function(signal, extraOpts) {
    print("signal: " + signal);

    var rt = new ReplTest("repl2tests");

    // implicit small oplog makes slave get out of sync
    m = rt.start(true, {oplogSize: "1"});
    s = rt.start(false, extraOpts);

    am = m.getDB("foo").a;

    am.save({_id: new ObjectId()});
    soonCount(1);
    assert.eq(0, s.getDB("admin").runCommand({"resync": 1}).ok);
    rt.stop(false, signal);

    big = new Array(2000).toString();
    for (i = 0; i < 1000; ++i)
        am.save({_id: new ObjectId(), i: i, b: big});

    s = rt.start(false, extraOpts, true);

    print("earliest op in master: " +
          tojson(m.getDB("local").oplog.$main.find().sort({$natural: 1}).limit(1).next()));
    print("latest op on slave: " + tojson(s.getDB("local").sources.findOne()));

    assert.soon(function() {
        var result = s.getDB("admin").runCommand({"resync": 1});
        print("resync says: " + tojson(result));
        return result.ok == 1;
    });

    soonCount(1001);
    assert.automsg("m.getDB( 'local' ).getCollection( 'oplog.$main' ).stats().size > 0");

    as = s.getDB("foo").a;
    assert.eq(1, as.find({i: 0}).count());
    assert.eq(1, as.find({i: 999}).count());

    assert.eq(0, s.getDB("admin").runCommand({"resync": 1}).ok);

    rt.stop();

};

doTest(15, {"vv": null});                // SIGTERM
doTest(9, {"vv": null, journal: null});  // SIGKILL
