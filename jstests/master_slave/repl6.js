// Test one master replicating to two slaves
//
// There is no automatic fail-over in a master/slave deployment, so if the master goes down, no new
// master will be elected. Therefore if the master is using an ephemeral storage engine, it cannot
// be restarted without losing all data. This test expects that restarting the master will maintain
// the node's data, so cannot be run with ephemeral storage engines.
// @tags: [requires_persistence]

var baseName = "jstests_repl6test";

soonCount = function(m, count) {
    assert.soon(function() {
        return m.getDB(baseName).a.find().count() == count;
    }, "expected count: " + count + " from : " + m);
};

doTest = function(signal) {

    ports = allocatePorts(3);

    ms1 = new ReplTest("repl6tests-1", [ports[0], ports[1]]);
    ms2 = new ReplTest("repl6tests-2", [ports[0], ports[2]]);

    m = ms1.start(true);
    s1 = ms1.start(false);
    s2 = ms2.start(false);

    am = m.getDB(baseName).a;

    for (i = 0; i < 1000; ++i)
        am.save({_id: new ObjectId(), i: i});

    soonCount(s1, 1000);
    soonCount(s2, 1000);

    check = function(as) {
        assert.eq(1, as.find({i: 0}).count());
        assert.eq(1, as.find({i: 999}).count());
    };

    as = s1.getDB(baseName).a;
    check(as);
    as = s2.getDB(baseName).a;
    check(as);

    ms1.stop(false, signal);
    ms2.stop(false, signal);

    for (i = 1000; i < 1010; ++i)
        am.save({_id: new ObjectId(), i: i});

    s1 = ms1.start(false, null, true);
    soonCount(s1, 1010);
    as = s1.getDB(baseName).a;
    assert.eq(1, as.find({i: 1009}).count());

    ms1.stop(true, signal);

    // Need to pause here on Windows, since killing processes does not synchronously close their
    // open file handles.
    sleep(5000);

    m = ms1.start(true, null, true);
    am = m.getDB(baseName).a;

    for (i = 1010; i < 1020; ++i)
        am.save({_id: new ObjectId(), i: i});

    soonCount(s1, 1020);
    assert.eq(1, as.find({i: 1019}).count());

    s2 = ms2.start(false, null, true);
    soonCount(s2, 1020);
    as = s2.getDB(baseName).a;
    assert.eq(1, as.find({i: 1009}).count());
    assert.eq(1, as.find({i: 1019}).count());

    ms1.stop();
    ms2.stop(false);
};

doTest(15);  // SIGTERM
