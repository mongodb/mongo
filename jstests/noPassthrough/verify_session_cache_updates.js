(function() {
    'use strict';

    // This test makes assertions about the number of sessions, which are not compatible with
    // implicit sessions.
    TestData.disableImplicitSessions = true;

    function runTest(conn) {
        for (var i = 0; i < 10; ++i) {
            conn.getDB("test").test.save({a: i});
        }

        function verify(conn, nRecords) {
            conn.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});
            assert.eq(nRecords, conn.getDB("config").system.sessions.find({}).count());
        }

        function getLastUse(conn) {
            conn.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});
            return conn.getDB("config").system.sessions.findOne({}).lastUse;
        }

        // initially we have no sessions
        verify(conn, 0);

        // Calling startSession in the shell doesn't initiate the session
        var session = conn.startSession();
        verify(conn, 0);

        // running a non-session updating command doesn't touch
        session.getDatabase("admin").runCommand("getLastError");
        verify(conn, 0);

        // running a session updating command does touch
        session.getDatabase("admin").runCommand({serverStatus: 1});
        verify(conn, 1);

        // running a session updating command updates last use
        {
            var lastUse = getLastUse(conn);
            sleep(200);
            session.getDatabase("admin").runCommand({serverStatus: 1});
            verify(conn, 1);
            assert.gt(getLastUse(conn), lastUse);
        }

        // verify that reading from a cursor updates last use
        {
            var cursor = session.getDatabase("test").test.find({}).batchSize(1);
            cursor.next();
            var lastUse = getLastUse(conn);
            sleep(200);
            verify(conn, 1);
            cursor.next();
            assert.gt(getLastUse(conn), lastUse);
        }

        session.endSession();
    }

    {
        var mongod = MongoRunner.runMongod({nojournal: ""});
        runTest(mongod);
        MongoRunner.stopMongod(mongod);
    }

    {
        var st = new ShardingTest({shards: 1, mongos: 1, config: 1});
        runTest(st.s0);
        st.stop();
    }
})();
