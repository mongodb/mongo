load('jstests/multiVersion/libs/multi_rs.js');
load('jstests/libs/os_helpers.js');

// Do not fail if this test leaves unterminated processes because this file expects replset1.js to
// throw for invalid SSL options.
TestData.failIfUnterminatedProcesses = false;

//=== Shared SSL testing library functions and constants ===

var KEYFILE = "jstests/libs/key1";
var SERVER_CERT = "jstests/libs/server.pem";
var CA_CERT = "jstests/libs/ca.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var DH_PARAM = "jstests/libs/8k-prime.dhparam";
var CLUSTER_CERT = "jstests/libs/cluster_cert.pem";

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
    // This will cause an assert.soon() in ReplSetTest to fail. This normally triggers the hang
    // analyzer, but since we do not want to run it on expected timeouts, we temporarily disable it.
    MongoRunner.runHangAnalyzer.disable();
    try {
        assert.throws(load, [replSetTestFile], "This setup should have failed");
    } finally {
        MongoRunner.runHangAnalyzer.enable();
    }
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
    assert.commandWorked(fooBulk.execute());
    assert.commandWorked(barBulk.execute());

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
        //
        // Also, the autosplitter will be turned on automatically with 'enableBalancer: true'. We
        // then want to disable the autosplitter, but cannot do so here with 'enableAutoSplit:
        // false' because ShardingTest will attempt to call disableAutoSplit(), which it will not be
        // authorized to do if auth is enabled.
        //
        // Once SERVER-14017 is fixed the "enableBalancer" line can be removed.

        // The mongo shell cannot authenticate as the internal __system user in tests that use x509
        // for cluster authentication. Choosing the default value for wcMajorityJournalDefault in
        // ReplSetTest cannot be done automatically without the shell performing such
        // authentication, so in this test we must make the choice explicitly, based on the global
        // test options.
        let wcMajorityJournalDefault;
        if (jsTestOptions().storageEngine == "inMemory") {
            wcMajorityJournalDefault = false;
        } else {
            wcMajorityJournalDefault = true;
        }

        var st = new ShardingTest({
            mongos: [options1],
            config: 1,
            shards: [options1, options2],
            other: {
                enableBalancer: true,
                configOptions: options1,
                writeConcernMajorityJournalDefault: wcMajorityJournalDefault
            },
        });

        // Create admin user in case the options include auth
        st.admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
        st.admin.auth('admin', 'pwd');

        authSucceeded = true;

        st.stopBalancer();
        st.disableAutoSplit();

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
        for (var i = 0; i < 128; i++) {
            bulk.insert({_id: i, string: bigstr});
        }
        assert.commandWorked(bulk.execute());
        assert.eq(128, db1.col.count(), "error retrieving documents from cluster");

        // Split chunk to make it small enough to move
        assert.commandWorked(st.splitFind("test.col", {_id: 0}));

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
            if (st.s.fullOptions.clusterAuthMode === 'x509') {
                // Index consistency check during shutdown needs a privileged user to auth as.
                const x509User =
                    'CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';
                st.s.getDB('$external')
                    .createUser({user: x509User, roles: [{role: '__system', db: 'admin'}]});
            }

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

function isMacOS(minVersion) {
    'use strict';

    function parseVersion(version) {
        // Intentionally leave the end of string unanchored.
        // This allows vesions like: 10.15.7-pl2 or other extra data.
        const v = version.match(/^(\d+)\.(\d+)\.(\d+)/);
        assert(v !== null, "Invalid version string '" + version + "'");
        return (v[1] << 16) | (v[2] << 8) | (v[3]);
    }

    const macOS = getBuildInfo().macOS;
    if (macOS === undefined) {
        // Not macOS at all.
        return false;
    }

    if (minVersion === undefined) {
        // Don't care what version, but it's macOS.
        return true;
    }

    assert(macOS.osProductVersion !== undefined,
           "Expected getBuildInfo() field 'macOS.osProductVersion' not present");
    return parseVersion(minVersion) <= parseVersion(macOS.osProductVersion);
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
        waitForConnect: true,
    });

    assert.eq(0,
              runMongoProgram('mongo',
                              '--ssl',
                              '--port',
                              conn.port,
                              '--sslPEMKeyFile',
                              'jstests/libs/client.pem',
                              '--sslCAFile',
                              'jstests/libs/ca.pem',
                              '--eval',
                              ';'));

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

function sslProviderSupportsTLS1_0() {
    if (isRHEL8()) {
        const cryptoPolicy = cat("/etc/crypto-policies/config");
        return cryptoPolicy.includes("LEGACY");
    }

    if (isOpenSSL3orGreater()) {
        return false;
    }

    return !isDebian10() && !isUbuntu2004();
}

function sslProviderSupportsTLS1_1() {
    if (isRHEL8()) {
        const cryptoPolicy = cat("/etc/crypto-policies/config");
        return cryptoPolicy.includes("LEGACY");
    }

    if (isOpenSSL3orGreater()) {
        return false;
    }

    return !isDebian10() && !isUbuntu2004();
}

function isOpenSSL3orGreater() {
    // Windows and macOS do not have "openssl.compiled" in buildInfo but they do have "running"
    const opensslCompiledIn = getBuildInfo().openssl.compiled !== undefined;
    if (!opensslCompiledIn) {
        return false;
    }

    return opensslVersionAsInt() > 0x3000000;
}

function opensslVersionAsInt() {
    const opensslInfo = getBuildInfo().openssl;
    if (!opensslInfo) {
        return null;
    }

    const matches = opensslInfo.running.match(/OpenSSL\s+(\d+)\.(\d+)\.(\d+)([a-z]?)/);
    assert.neq(matches, null);

    let version = (matches[1] << 24) | (matches[2] << 16) | (matches[3] << 8);

    return version;
}

function copyCertificateFile(a, b) {
    if (_isWindows()) {
        // correctly replace forward slashes for Windows
        a = a.replace(/\//g, "\\");
        b = b.replace(/\//g, "\\");
        assert.eq(0, runProgram("cmd.exe", "/c", "copy", a, b));
        return;
    }
    assert.eq(0, runProgram("cp", a, b));
}
