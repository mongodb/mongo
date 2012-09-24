/**
 * Testing read preference on DBClientReplicaSets, specifically on the auto-retry
 * and automatic failover selection
 */

function basicTest() {
    var replTest = new ReplSetTest({ name: 'basic', nodes: 2, useHostName: true });
    replTest.startSet({ oplogSize: 1 });
    replTest.initiate();
    replTest.awaitSecondaryNodes();

    var PRI_HOST = replTest.getPrimary().host;
    var SEC_HOST = replTest.getSecondary().host;

    var replConn = new Mongo(replTest.getURL());
    var coll = replConn.getDB('test').user;
    var dest = coll.find().readPref('primary').explain().server;
    assert.eq(PRI_HOST, dest);

    // Create brand new connection to make sure that the last cached is not used
    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('secondary').explain().server;
    assert.eq(SEC_HOST, dest);

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('primaryPreferred').explain().server;
    assert.eq(PRI_HOST, dest);

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('secondaryPreferred').explain().server;
    assert.eq(SEC_HOST, dest);

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    // just make sure that it doesn't throw
    coll.find().readPref('nearest').explain();

    replTest.stopSet();
}

function noPriNoSecTest() {
    var replTest = new ReplSetTest({ name: 'priOnlyNoPri', useHostName: true,
        nodes: [{}, { arbiter: true }, { arbiter: true }]});
    replTest.startSet({ oplogSize: 1 });
    replTest.initiate();
    replTest.awaitSecondaryNodes();

    var replConn = new Mongo(replTest.getURL());
    var coll = replConn.getDB('test').user;

    replTest.stop(0);

    assert.throws(function() {
        coll.find().readPref('primary').explain();
    });

    // Make sure that it still fails even when trying to refresh
    assert.throws(function() {
        coll.find().readPref('primary').explain();
    });

    // Don't need to create new connection because failed connections
    // would never be reused, and also becasue the js Mongo contructor
    // will throw when it can't connect to a primary
    assert.throws(function() {
        coll.find().readPref('secondary').explain();
    });

    assert.throws(function() {
        coll.find().readPref('secondary').explain();
    });

    assert.throws(function() {
        coll.find().readPref('primaryPreferred').explain();
    });

    assert.throws(function() {
        coll.find().readPref('primaryPreferred').explain();
    });

    assert.throws(function() {
        coll.find().readPref('secondaryPreferred').explain();
    });

    assert.throws(function() {
        coll.find().readPref('secondaryPreferred').explain();
    });

    assert.throws(function() {
        coll.find().readPref('neareset').explain();
    });

    assert.throws(function() {
        coll.find().readPref('nearest').explain();
    });

    replTest.stopSet();
}

function priOkNoSecTest() {
    var replTest = new ReplSetTest({ name: 'secOnlyNoSec', useHostName: true,
        nodes: [{}, { arbiter: true }, {}]});
    replTest.startSet({ oplogSize: 1 });
    replTest.initiate();
    replTest.awaitSecondaryNodes();

    var replConn = new Mongo(replTest.getURL());
    var coll = replConn.getDB('test').user;

    replTest.stop(2);

    var PRI_HOST = replTest.getPrimary().host;

    var dest = coll.find().readPref('primary').explain().server;
    assert.eq(PRI_HOST, dest);

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('primaryPreferred').explain().server;
    assert.eq(PRI_HOST, dest);

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    assert.throws(function() {
        coll.find().readPref('secondary').explain();
    });

    assert.throws(function() {
        coll.find().readPref('secondary').explain();
    });

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('secondaryPreferred').explain().server;
    assert.eq(PRI_HOST, dest);

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('nearest').explain().server;
    assert.eq(PRI_HOST, dest);

    replTest.stopSet();
}

function noPriSecOkTest() {
    var replTest = new ReplSetTest({ name: 'priPrefNoPri', useHostName: true,
        nodes: [{ }, { arbiter: true }, { }]});
    replTest.startSet({ oplogSize: 1 });
    replTest.initiate();
    replTest.awaitSecondaryNodes();

    var priConn = replTest.getPrimary();
    var conf = priConn.getDB('local').system.replset.findOne();
    conf.version++;
    conf.members[0].priority = 99;
    conf.members[2].priority = 0;

    var SEC_HOST = replTest.nodes[2].host;

    try {
        priConn.getDB('admin').runCommand({ replSetReconfig: conf });
    } catch (x) {
        print('Exception from reconfig: ' + x);
    }

    var replConn = new Mongo(replTest.getURL());
    var coll = replConn.getDB('test').user;

    replTest.stop(0);

    assert.throws(function() {
        coll.find().readPref('primary').explain();
    });

    // Make sure that it still fails even when trying to refresh
    assert.throws(function() {
        coll.find().readPref('primary').explain();
    });

    replConn = new Mongo(replTest.getURL());
    coll = replConn.getDB('test').user;
    var dest = coll.find().readPref('primaryPreferred').explain().server;
    assert.eq(SEC_HOST, dest);

    replTest.start(0, {}, true);
    replTest.awaitSecondaryNodes();
    replConn = new Mongo(replTest.getURL());
    replTest.stop(0);
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('secondary').explain().server;
    assert.eq(SEC_HOST, dest);

    replTest.start(0, {}, true);
    replTest.awaitSecondaryNodes();
    replConn = new Mongo(replTest.getURL());
    replTest.stop(0);
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('secondaryPreferred').explain().server;
    assert.eq(SEC_HOST, dest);

    replTest.start(0, {}, true);
    replTest.awaitSecondaryNodes();
    replConn = new Mongo(replTest.getURL());
    replTest.stop(0);
    coll = replConn.getDB('test').user;
    dest = coll.find().readPref('nearest').explain().server;
    assert.eq(SEC_HOST, dest);

    replTest.stopSet();
}

basicTest();
noPriNoSecTest();
priOkNoSecTest();
noPriSecOkTest();

