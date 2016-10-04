/**
 * Test that verifies client metadata is logged into log file on new connections.
 */
(function() {
    'use strict';

    let checkLog = function(conn) {
        let coll = conn.getCollection("test.foo");
        assert.writeOK(coll.insert({_id: 1}));

        print(`Checking ${conn.fullOptions.logFile} for client metadata message`);
        let log = cat(conn.fullOptions.logFile);

        assert(
            /received client metadata from .*: { application: { name: ".*" }, driver: { name: ".*", version: ".*" }, os: { type: ".*", name: ".*", architecture: ".*", version: ".*" } }/
                .test(log),
            "'received client metadata' log line missing in log file!\n" + "Log file contents: " +
                conn.fullOptions.logFile +
                "\n************************************************************\n" + log +
                "\n************************************************************");
    };

    // Test MongoD
    let testMongoD = function() {
        let conn = MongoRunner.runMongod({useLogFiles: true});
        assert.neq(null, conn, 'mongod was unable to start up');

        checkLog(conn);

        MongoRunner.stopMongod(conn);
    };

    // Test MongoS
    let testMongoS = function() {
        let options = {
            mongosOptions: {useLogFiles: true},
        };

        let st = new ShardingTest({shards: 1, mongos: 1, other: options});

        checkLog(st.s0);

        st.stop();
    };

    testMongoD();
    testMongoS();
})();
