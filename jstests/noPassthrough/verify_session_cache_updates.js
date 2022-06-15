// @tags: [requires_sharding]

(function() {
'use strict';

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

function runTest(conn) {
    for (var i = 0; i < 10; ++i) {
        conn.getDB("test").test.save({a: i});
    }

    function countSessions(conn, since) {
        conn.adminCommand({refreshLogicalSessionCacheNow: 1});
        return conn.getDB("config").system.sessions.countDocuments({lastUse: {"$gt": since}});
    }

    function getLatestSessionTime(conn) {
        conn.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});
        let lastSession = conn.getDB("config")
                              .system.sessions.aggregate([{"$sort": {lastUse: -1}}, {$limit: 1}])
                              .toArray();
        return (lastSession.length ? lastSession[0].lastUse : new Date(0));
    }

    let origSessTime = getLatestSessionTime(conn);

    // initially we have no sessions
    assert.eq(0, countSessions(conn, origSessTime));

    // Calling startSession in the shell doesn't initiate the session
    var session = conn.startSession();
    assert.eq(0, countSessions(conn, origSessTime));

    // running a command that doesn't require auth does touch
    session.getDatabase("admin").runCommand("hello");
    assert.eq(1, countSessions(conn, origSessTime));

    // running a session updating command does touch
    session.getDatabase("admin").runCommand({serverStatus: 1});
    assert.eq(1, countSessions(conn, origSessTime));

    // running a session updating command updates last use
    {
        var lastUse = getLatestSessionTime(conn);
        sleep(200);
        session.getDatabase("admin").runCommand({serverStatus: 1});
        assert.eq(1, countSessions(conn, origSessTime));
        assert.gt(getLatestSessionTime(conn), lastUse);
    }

    // verify that reading from a cursor updates last use
    {
        var cursor = session.getDatabase("test").test.find({}).batchSize(1);
        cursor.next();
        var lastUse = getLatestSessionTime(conn);
        sleep(200);
        assert.eq(1, countSessions(conn, origSessTime));
        cursor.next();
        assert.gt(getLatestSessionTime(conn), lastUse);
    }

    session.endSession();
}

{
    var mongod = MongoRunner.runMongod();
    runTest(mongod);
    MongoRunner.stopMongod(mongod);
}

{
    var st = new ShardingTest({shards: 1, mongos: 1, config: 1});
    st.rs0.getPrimary().getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});

    runTest(st.s0);
    st.stop();
}
})();
