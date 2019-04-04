/**
 * Test that verifies client metadata is logged into log file on new connections.
 * @tags: [requires_sharding]
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
        assert.neq(null, conn, 'merizod was unable to start up');

        checkLog(conn);

        MongoRunner.stopMongod(conn);
    };

    // Test MongoS
    let testMongoS = function() {
        let options = {
            merizosOptions: {useLogFiles: true},
        };

        let st = new ShardingTest({shards: 1, merizos: 1, other: options});

        checkLog(st.s0);

        // Validate db.currentOp() contains merizos information
        let curOp = st.s0.adminCommand({currentOp: 1});
        print(tojson(curOp));

        var inprogSample = null;
        for (let inprog of curOp.inprog) {
            if (inprog.hasOwnProperty("clientMetadata") &&
                inprog.clientMetadata.hasOwnProperty("merizos")) {
                inprogSample = inprog;
                break;
            }
        }

        assert.neq(inprogSample.clientMetadata.merizos.host, "unknown");
        assert.neq(inprogSample.clientMetadata.merizos.client, "unknown");
        assert.neq(inprogSample.clientMetadata.merizos.version, "unknown");

        st.stop();
    };

    testMongoD();
    testMongoS();
})();
