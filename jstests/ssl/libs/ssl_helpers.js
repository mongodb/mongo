import "jstests/multiVersion/libs/multi_rs.js";

import {isDebian, isRHEL8, isUbuntu, isUbuntu2004} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {basicReplsetTest} from "jstests/replsets/libs/basic_replset_test.js";

// Do not fail if this test leaves unterminated processes because this file expects replset1.js to
// throw for invalid SSL options.
TestData.ignoreUnterminatedProcesses = true;

//=== Shared SSL testing library functions and constants ===

export var KEYFILE = "jstests/libs/key1";

export var SERVER_CERT = "jstests/libs/server.pem";
export var TRUSTED_SERVER_CERT = "jstests/libs/trusted-server.pem";
export var CA_CERT = "jstests/libs/ca.pem";
export var TRUSTED_CA_CERT = "jstests/libs/trusted-ca.pem";
export var CLIENT_CERT = "jstests/libs/client.pem";
export var TRUSTED_CLIENT_CERT = "jstests/libs/trusted-client.pem";
export var DH_PARAM = "jstests/libs/8k-prime.dhparam";
export var CLUSTER_CERT = "jstests/libs/cluster_cert.pem";

// Note: "tlsAllowInvalidCertificates" is enabled to avoid
// hostname conflicts with our testing certificates
export var disabled = {
    tlsMode: "disabled",
};

export var allowTLS = {
    tlsMode: "allowTLS",
    tlsAllowInvalidCertificates: "",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
};

export var preferTLS = {
    tlsMode: "preferTLS",
    tlsAllowInvalidCertificates: "",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
};

export var requireTLS = {
    tlsMode: "requireTLS",
    tlsAllowInvalidCertificates: "",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
};

export var dhparamSSL = {
    tlsMode: "requireTLS",
    tlsAllowInvalidCertificates: "",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    setParameter: {"opensslDiffieHellmanParameters": DH_PARAM},
};

// Test if ssl replset  configs work

export var replShouldSucceed = function (name, opt1, opt2) {
    // try running this file using the given config
    basicReplsetTest(15, opt1, opt2, name);
};

// Test if ssl replset configs fail
export var replShouldFail = function (name, opt1, opt2) {
    // This will cause an assert.soon() in ReplSetTest to fail. This normally triggers the hang
    // analyzer, but since we do not want to run it on expected timeouts, we temporarily disable it.
    MongoRunner.runHangAnalyzer.disable();
    try {
        assert.throws(() => basicReplsetTest(15, opt1, opt2, name));
    } finally {
        MongoRunner.runHangAnalyzer.enable();
    }
    // Note: this leaves running mongod processes.
};

/**
 * Test that $lookup works with a sharded source collection. This is tested because of
 * the connections opened between mongos/shards and between the shards themselves.
 */
export function testShardedLookup(shardingTest) {
    let st = shardingTest;
    assert(st.adminCommand({enableSharding: "lookupTest"}), "error enabling sharding for this configuration");
    assert(
        st.adminCommand({shardCollection: "lookupTest.foo", key: {_id: "hashed"}}),
        "error sharding collection for this configuration",
    );

    let lookupdb = st.getDB("lookupTest");

    // insert a few docs to ensure there are documents on multiple shards.
    let fooBulk = lookupdb.foo.initializeUnorderedBulkOp();
    let barBulk = lookupdb.bar.initializeUnorderedBulkOp();
    let lookupShouldReturn = [];
    for (let i = 0; i < 64; i++) {
        fooBulk.insert({_id: i});
        barBulk.insert({_id: i});
        lookupShouldReturn.push({_id: i, bar_docs: [{_id: i}]});
    }
    assert.commandWorked(fooBulk.execute());
    assert.commandWorked(barBulk.execute());

    let docs = lookupdb.foo
        .aggregate([
            {$sort: {_id: 1}},
            {$lookup: {from: "bar", localField: "_id", foreignField: "_id", as: "bar_docs"}},
        ])
        .toArray();
    assert.eq(lookupShouldReturn, docs, "error $lookup failed in this configuration");
    assert.commandWorked(lookupdb.dropDatabase());
}

/**
 * Takes in two mongod/mongos configuration options and runs a basic
 * sharding test to see if they can work together...
 */
export function mixedShardTest(options1, options2, shouldSucceed) {
    let authSucceeded = false;
    try {
        // TODO SERVER-14017 is fixed the "enableBalancer" line can be removed.
        // Start ShardingTest with enableBalancer because ShardingTest attempts to turn
        // off the balancer otherwise, which it will not be authorized to do if auth is enabled.

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
            shouldFailInit: !shouldSucceed,
            other: {
                enableBalancer: true,
                configOptions: options1,
                writeConcernMajorityJournalDefault: wcMajorityJournalDefault,
            },
        });

        // Create admin user in case the options include auth
        st.admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
        st.admin.auth("admin", "pwd");

        authSucceeded = true;

        st.stopBalancer();

        // Test that $lookup works because it causes outgoing connections to be opened
        testShardedLookup(st);

        // Test mongos talking to config servers
        let r = st.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName});
        assert.eq(r, true, "error enabling sharding for this configuration");

        r = st.adminCommand({movePrimary: "test", to: st.shard1.shardName});
        assert.eq(r, true, "error movePrimary failed for this configuration");

        let db1 = st.getDB("test");
        r = st.adminCommand({shardCollection: "test.col", key: {_id: 1}});
        assert.eq(r, true, "error sharding collection for this configuration");

        // Test mongos talking to shards
        let bigstr = "#".repeat(1024 * 1024);

        let bulk = db1.col.initializeUnorderedBulkOp();
        for (let i = 0; i < 128; i++) {
            bulk.insert({_id: i, string: bigstr});
        }
        assert.commandWorked(bulk.execute());
        assert.eq(128, db1.col.count(), "error retrieving documents from cluster");

        // Split chunk to make it small enough to move
        assert.commandWorked(st.splitFind("test.col", {_id: 0}));

        // Test shards talking to each other
        r = st.getDB("test").adminCommand({moveChunk: "test.col", find: {_id: 0}, to: st.shard0.shardName});
        assert(r.ok, "error moving chunks: " + tojson(r));

        db1.col.remove({});
    } catch (e) {
        if (shouldSucceed) throw e;
        // silence error if we should fail...
        print("IMPORTANT! => Test failed when it should have failed...continuing...");
    } finally {
        // Authenticate csrs so ReplSetTest.stopSet() can do db hash check.
        if (authSucceeded && st.configRS) {
            st.configRS.nodes.forEach((node) => {
                node.getDB("admin").auth("admin", "pwd");
            });
        }

        // This has to be done in order for failure
        // to not prevent future tests from running...
        if (st) {
            if (st.s.fullOptions.clusterAuthMode === "x509") {
                // Index consistency check during shutdown needs a privileged user to auth as.
                const x509User = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";
                st.s.getDB("$external").createUser({user: x509User, roles: [{role: "__system", db: "admin"}]});
            }

            st.stop();
        }
    }
}

export function determineSSLProvider() {
    const info = getBuildInfo();
    const ssl = info.openssl === undefined ? "" : info.openssl.running;
    if (/OpenSSL/.test(ssl)) {
        return "openssl";
    } else if (/Apple/.test(ssl)) {
        return "apple";
    } else if (/Windows/.test(ssl)) {
        return "windows";
    } else {
        return null;
    }
}

export function isMacOS(minVersion) {
    function parseVersion(version) {
        // Intentionally leave the end of string unanchored.
        // This allows vesions like: 10.15.7-pl2 or other extra data.
        const v = version.match(/^(\d+)\.(\d+)\.(\d+)/);
        assert(v !== null, "Invalid version string '" + version + "'");
        return (v[1] << 16) | (v[2] << 8) | v[3];
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

    assert(macOS.osProductVersion !== undefined, "Expected getBuildInfo() field 'macOS.osProductVersion' not present");
    return parseVersion(minVersion) <= parseVersion(macOS.osProductVersion);
}

export function requireSSLProvider(required, fn) {
    if (typeof required === "string") {
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

export function detectDefaultTLSProtocol() {
    const conn = MongoRunner.runMongod({
        tlsMode: "allowTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsDisabledProtocols: "none",
        useLogFiles: true,
        tlsLogVersions: "TLS1_0,TLS1_1,TLS1_2,TLS1_3",
        waitForConnect: true,
        tlsCAFile: CA_CERT,
    });

    assert.eq(
        0,
        runMongoProgram(
            "mongo",
            "--ssl",
            "--port",
            conn.port,
            "--tlsCertificateKeyFile",
            CLIENT_CERT,
            "--tlsCAFile",
            CA_CERT,
            "--eval",
            ";",
        ),
    );

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

export function sslProviderSupportsTLS1_0() {
    if (isRHEL8()) {
        const cryptoPolicy = cat("/etc/crypto-policies/config");
        return cryptoPolicy.includes("LEGACY");
    }

    if (isOpenSSL3orGreater()) {
        return false;
    }

    return !isDebian() && !isUbuntu2004();
}

export function sslProviderSupportsTLS1_1() {
    if (isRHEL8()) {
        const cryptoPolicy = cat("/etc/crypto-policies/config");
        return cryptoPolicy.includes("LEGACY");
    }

    if (isOpenSSL3orGreater()) {
        return false;
    }

    return !isDebian() && !isUbuntu2004();
}

export function isOpenSSL3orGreater() {
    // Windows and macOS do not have "openssl.compiled" in buildInfo but they do have "running"
    const opensslCompiledIn = getBuildInfo().openssl.compiled !== undefined;
    if (!opensslCompiledIn) {
        return false;
    }

    return opensslVersionAsInt() >= 0x3000000;
}

export function opensslVersionAsInt() {
    const opensslInfo = getBuildInfo().openssl;
    if (!opensslInfo || !opensslInfo.compiled) {
        // openssl undefined if apple or windows
        return undefined;
    }

    const matches = opensslInfo.running.match(/OpenSSL\s+(\d+)\.(\d+)\.(\d+)([a-z]?)/);
    assert.neq(matches, null, "cannot parse openSSL version from '" + opensslInfo.running + "'");

    let version = (matches[1] << 24) | (matches[2] << 16) | (matches[3] << 8);

    return version;
}

export function supportsFIPS() {
    // OpenSSL supports FIPS
    let expectSupportsFIPS = determineSSLProvider() == "openssl";

    // But OpenSSL supports FIPS only sometimes
    // - Debian does not support FIPS, Fedora 37 does not, Fedora 38 does
    // - Ubuntu only supports FIPS with Ubuntu pro
    if (expectSupportsFIPS) {
        if (isDebian() || isUbuntu()) {
            expectSupportsFIPS = false;
        }
    }

    return expectSupportsFIPS;
}

export function copyCertificateFile(a, b) {
    if (_isWindows()) {
        // correctly replace forward slashes for Windows
        a = a.replace(/\//g, "\\");
        b = b.replace(/\//g, "\\");
        assert.eq(0, runProgram("cmd.exe", "/c", "copy", a, b));
        return;
    }
    assert.eq(0, runProgram("cp", a, b));
}

export function clientSupportsTLS1_1() {
    if (!sslProviderSupportsTLS1_1()) {
        return false;
    }
    const opensslVersion = opensslVersionAsInt();
    return opensslVersion === undefined ? true : opensslVersion >= 0x100000c; // 1.0.0l
}

export function clientSupportsTLS1_2() {
    const opensslVersion = opensslVersionAsInt();
    return opensslVersion === undefined ? true : opensslVersion >= 0x1000106; // 1.0.1f
}

export function clientSupportsTLS1_3() {
    // SERVER-98279: support tls 1.3 for windows & apple
    if (determineSSLProvider() === "apple" || determineSSLProvider() === "windows") {
        return false;
    }
    const opensslVersion = opensslVersionAsInt();
    return opensslVersion === undefined ? true : opensslVersion >= 0x1010100; // 1.1.1
}
