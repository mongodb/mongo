load('jstests/multiVersion/libs/multi_rs.js');

// Do not fail if this test leaves unterminated processes because this file expects replset1.js to
// throw for invalid SSL options.
TestData.failIfUnterminatedProcesses = false;

//=== Shared SSL testing library functions and constants ===

var KEYFILE = "jstests/libs/key1";
var SERVER_CERT = "jstests/libs/server.pem";
var CA_CERT = "jstests/libs/ca.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var DH_PARAM = "jstests/libs/8k-prime.dhparam";

// Note: "sslAllowInvalidCertificates" is enabled to avoid
// hostname conflicts with our testing certificates
var disabled = {sslMode: "disabled"};
var allowSSL = {
    sslMode: "allowSSL",
    sslAllowInvalidCertificates: "",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT
};
var preferSSL = {
    sslMode: "preferSSL",
    sslAllowInvalidCertificates: "",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT
};
var requireSSL = {
    sslMode: "requireSSL",
    sslAllowInvalidCertificates: "",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT
};

var dhparamSSL = {
    sslMode: "requireSSL",
    sslAllowInvalidCertificates: "",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT,
    setParameter: {"opensslDiffieHellmanParameters": DH_PARAM}
};

// Test if ssl replset  configs work

var replSetTestFile = "jstests/replsets/replset1.js";

var replShouldSucceed = function(name, opt1, opt2) {
    ssl_options1 = opt1;
    ssl_options2 = opt2;
    ssl_name = name;
    // try running this file using the given config
    load(replSetTestFile);
};

// Test if ssl replset configs fail
var replShouldFail = function(name, opt1, opt2) {
    ssl_options1 = opt1;
    ssl_options2 = opt2;
    ssl_name = name;
    assert.throws(load, [replSetTestFile], "This setup should have failed");
    // Note: this leaves running mongod processes.
};

/**
 * Test that $lookup works with a sharded source collection. This is tested because of
 * the connections opened between mongos/shards and between the shards themselves.
 */
function testShardedLookup(shardingTest) {
    var st = shardingTest;
    assert(st.adminCommand({enableSharding: "lookupTest"}),
           "error enabling sharding for this configuration");
    assert(st.adminCommand({shardCollection: "lookupTest.foo", key: {_id: "hashed"}}),
           "error sharding collection for this configuration");

    var lookupdb = st.getDB("lookupTest");

    // insert a few docs to ensure there are documents on multiple shards.
    var fooBulk = lookupdb.foo.initializeUnorderedBulkOp();
    var barBulk = lookupdb.bar.initializeUnorderedBulkOp();
    var lookupShouldReturn = [];
    for (var i = 0; i < 64; i++) {
        fooBulk.insert({_id: i});
        barBulk.insert({_id: i});
        lookupShouldReturn.push({_id: i, bar_docs: [{_id: i}]});
    }
    assert.writeOK(fooBulk.execute());
    assert.writeOK(barBulk.execute());

    var docs =
        lookupdb.foo
            .aggregate([
                {$sort: {_id: 1}},
                {$lookup: {from: "bar", localField: "_id", foreignField: "_id", as: "bar_docs"}}
            ])
            .toArray();
    assert.eq(lookupShouldReturn, docs, "error $lookup failed in this configuration");
    assert.commandWorked(lookupdb.dropDatabase());
}

/**
 * Takes in two mongod/mongos configuration options and runs a basic
 * sharding test to see if they can work together...
 */
function mixedShardTest(options1, options2, shouldSucceed) {
    let authSucceeded = false;
    try {
        // Start ShardingTest with enableBalancer because ShardingTest attempts to turn
        // off the balancer otherwise, which it will not be authorized to do if auth is enabled.
        // Once SERVER-14017 is fixed the "enableBalancer" line can be removed.
        // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
        var st = new ShardingTest({
            mongos: [options1],
            config: [options1],
            shards: [options1, options2],
            other: {enableBalancer: true, shardAsReplicaSet: false}
        });

        // Create admin user in case the options include auth
        st.admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
        st.admin.auth('admin', 'pwd');

        authSucceeded = true;

        st.stopBalancer();

        // Test that $lookup works because it causes outgoing connections to be opened
        testShardedLookup(st);

        // Test mongos talking to config servers
        var r = st.adminCommand({enableSharding: "test"});
        assert.eq(r, true, "error enabling sharding for this configuration");

        st.ensurePrimaryShard("test", st.shard0.shardName);
        r = st.adminCommand({movePrimary: 'test', to: st.shard1.shardName});
        assert.eq(r, true, "error movePrimary failed for this configuration");

        var db1 = st.getDB("test");
        r = st.adminCommand({shardCollection: "test.col", key: {_id: 1}});
        assert.eq(r, true, "error sharding collection for this configuration");

        // Test mongos talking to shards
        var bigstr = Array(1024 * 1024).join("#");

        var bulk = db1.col.initializeUnorderedBulkOp();
        for (var i = 0; i < 64; i++) {
            bulk.insert({_id: i, string: bigstr});
        }
        assert.writeOK(bulk.execute());
        assert.eq(64, db1.col.count(), "error retrieving documents from cluster");

        // Test shards talking to each other
        r = st.getDB('test').adminCommand(
            {moveChunk: 'test.col', find: {_id: 0}, to: st.shard0.shardName});
        assert(r.ok, "error moving chunks: " + tojson(r));

        db1.col.remove({});

    } catch (e) {
        if (shouldSucceed)
            throw e;
        // silence error if we should fail...
        print("IMPORTANT! => Test failed when it should have failed...continuing...");
    } finally {
        // Authenticate csrs so ReplSetTest.stopSet() can do db hash check.
        if (authSucceeded && st.configRS) {
            st.configRS.nodes.forEach((node) => {
                node.getDB('admin').auth('admin', 'pwd');
            });
        }
        // This has to be done in order for failure
        // to not prevent future tests from running...
        if (st) {
            st.stop();
        }
    }
}

function determineSSLProvider() {
    'use strict';
    const info = getBuildInfo();
    const ssl = (info.openssl === undefined) ? '' : info.openssl.running;
    if (/OpenSSL/.test(ssl)) {
        return 'openssl';
    } else if (/Apple/.test(ssl)) {
        return 'apple';
    } else if (/Windows/.test(ssl)) {
        return 'windows';
    } else {
        return null;
    }
}

function requireSSLProvider(required, fn) {
    'use strict';
    if ((typeof required) === 'string') {
        required = [required];
    }

    const provider = determineSSLProvider();
    if (!required.includes(provider)) {
        print("*****************************************************");
        print("Skipping " + tojson(required) + " test because SSL provider is " + provider);
        print("*****************************************************");
        return;
    }
    fn();
}

function detectDefaultTLSProtocol() {
    const conn = MongoRunner.runMongod({
        sslMode: 'allowSSL',
        sslPEMKeyFile: SERVER_CERT,
        sslDisabledProtocols: 'none',
        useLogFiles: true,
        tlsLogVersions: "TLS1_0,TLS1_1,TLS1_2,TLS1_3",
    });

    const res = conn.getDB("admin").serverStatus().transportSecurity;

    MongoRunner.stopMongod(conn);

    // Verify that the default protocol is either TLS1.2 or TLS1.3.
    // No supported platform should default to an older protocol version.
    assert.eq(0, res["1.0"]);
    assert.eq(0, res["1.1"]);
    assert.eq(0, res["unknown"]);
    assert.neq(res["1.2"], res["1.3"]);

    if (res["1.2"].tojson() != NumberLong(0).tojson()) {
        return "TLS1_2";
    } else {
        return "TLS1_3";
    }
}
