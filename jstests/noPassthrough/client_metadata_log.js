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

    // Test BongoD
    let testBongoD = function() {
        let conn = BongoRunner.runBongod({useLogFiles: true});
        assert.neq(null, conn, 'bongod was unable to start up');

        checkLog(conn);

        BongoRunner.stopBongod(conn);
    };

    // Test BongoS
    let testBongoS = function() {
        let options = {
            bongosOptions: {useLogFiles: true},
        };

        let st = new ShardingTest({shards: 1, bongos: 1, other: options});

        checkLog(st.s0);

        st.stop();
    };

    testBongoD();
    testBongoS();
})();
