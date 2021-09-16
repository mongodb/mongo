/**
 * Validate client side scram cache is used
 *
 * Ensure that for each cache miss, an entry is added
 *
 * @tags: [requires_sharding]
 */
(function() {
'use strict';

function setup(conn) {
    const adminDB = conn.getDB('admin');

    adminDB.createUser({user: 'root', pwd: "pwd", roles: ["root", "clusterMonitor"]});
}

function runTests(conn) {
    const adminDB = conn.getDB('admin');

    assert.eq(1, adminDB.auth('root', "pwd"));

    adminDB.collTest.insert({x: 1});

    const ss = adminDB.serverStatus();
    jsTestLog(tojson(ss));

    const sc = ss.scramCache;

    // Validate that for each miss, we get an entry
    // If the cache was not used, the missed count would exceed the count of entries by a
    // significant margin
    assert.gte(sc["SCRAM-SHA-1"].count + 2, sc["SCRAM-SHA-1"].misses);

    assert.gte(sc["SCRAM-SHA-256"].count + 2, sc["SCRAM-SHA-256"].misses);

    assert(sc["SCRAM-SHA-1"].count > 0 || sc["SCRAM-SHA-256"].count > 0,
           "Cache was not used at all");

    adminDB.logout();
}

const st = new ShardingTest({shards: 1, keyFile: 'jstests/libs/key1'});
setup(st.s);

// Validate mongos
runTests(st.s);

// Validate a RS member
runTests(st.configRS.getPrimary());

st.stop();
})();
