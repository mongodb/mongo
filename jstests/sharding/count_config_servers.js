/**
 * Test count commands against the config servers, including when some of them are down.
 * This test fails when run with authentication due to SERVER-6327
 */
(function() {
    "use strict";

    var st = new ShardingTest({name: 'sync_conn_cmd', shards: 0});
    st.s.setSlaveOk(true);

    var configDB = st.config;
    var coll = configDB.test;

    for (var x = 0; x < 10; x++) {
        assert.writeOK(coll.insert({v: x}));
    }

    if (st.configRS) {
        // Make sure the inserts are replicated to all config servers.
        st.configRS.awaitReplication();
    }

    var testNormalCount = function() {
        var cmdRes = configDB.runCommand({count: coll.getName()});
        assert(cmdRes.ok);
        assert.eq(10, cmdRes.n);
    };

    var testCountWithQuery = function() {
        var cmdRes = configDB.runCommand({count: coll.getName(), query: {v: {$gt: 6}}});
        assert(cmdRes.ok);
        assert.eq(3, cmdRes.n);
    };

    // Use invalid query operator to make the count return error
    var testInvalidCount = function() {
        var cmdRes = configDB.runCommand({count: coll.getName(), query: {$c: {$abc: 3}}});
        assert(!cmdRes.ok);
        assert(cmdRes.errmsg.length > 0);
    };

    // Test with all config servers up
    testNormalCount();
    testCountWithQuery();
    testInvalidCount();

    // Test with the first config server down
    MongoRunner.stopMongod(st.c0);

    testNormalCount();
    testCountWithQuery();
    testInvalidCount();

    // Test with the first and second config server down
    MongoRunner.stopMongod(st.c1);
    jsTest.log('Second server is down');

    testNormalCount();
    testCountWithQuery();
    testInvalidCount();

    st.stop();

}());
