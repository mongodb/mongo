// Test that a client can authenticate against the server with roles.
// Also validates RFC2253
load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

    // Amazon linux does not currently support ECDHE
    const EXCLUDED_BUILDS = ['amazon', 'amzn64'];

    const suites = [
        "SSLV2 Cipher Suites",
        "SSLV3 Cipher Suites",
        "TLSV1_0 Cipher Suites",
        "TLSV1_1 Cipher Suites",
        "TLSV1_2 Cipher Suites",
        "TLSV1_3 Cipher Suites"
    ];
    const SERVER_CERT = "jstests/libs/server.pem";

    function runSSLYze(port) {
        let target_os = buildInfo().buildEnvironment.target_os;
        let target_arch = buildInfo().buildEnvironment.target_arch;

        if (target_os === "macOS" || target_arch !== "x86_64") {
            return null;
        }

        let python = "/usr/bin/env python3";
        let sslyze = " jstests/ssl/sslyze_tester.py ";

        if (_isWindows()) {
            const paths = ["c:\\python36\\python.exe", "c:\\python\\python36\\python.exe"];
            for (let p of paths) {
                if (fileExists(p)) {
                    python = p;
                }
            }
        }

        const python_command = python + sslyze + "--port=" + port;
        let ret = 0;
        if (_isWindows()) {
            ret = runProgram('cmd.exe', '/c', python_command);
        } else {
            ret = runProgram('/bin/sh', '-c', python_command);
        }
        assert.eq(ret, 0);

        try {
            let ciphers = cat("jstests/ssl/ciphers.json");
            return JSON.parse(ciphers);
        } catch (e) {
            jsTestLog("Failed to parse ciphers.json");
            throw e;
        } finally {
            const python_delete_command = python + sslyze + "--delete";
            if (_isWindows()) {
                ret = runProgram('cmd.exe', '/c', python_delete_command);
            } else {
                ret = runProgram('/bin/sh', '-c', python_delete_command);
            }
            assert.eq(ret, 0);
        }
    }

    function testSSLYzeOutput(cipherDict) {
        // Checking that SSL 1.0, 2.0, 3.0 and TLS 1.0 are not accepted
        suites.slice(0, 3).forEach(tlsVersion => assert(cipherDict[tlsVersion].length === 0));

        // Printing TLS 1.1, 1.2, and 1.3 suites that are accepted
        let hasECDHE = false;
        suites.slice(3, 6).forEach(tlsVersion => {
            print('*************************\n' + tlsVersion + ": ");
            cipherDict[tlsVersion].forEach(cipher => {
                print(cipher);
                if (cipher.includes('ECDHE'))
                    hasECDHE = true;
            });
        });

        // All platforms except Amazon Linux 1 should support ECDHE
        if (!EXCLUDED_BUILDS.includes(buildInfo().buildEnvironment.distmod)) {
            assert(hasECDHE, 'Supports at least one ECDHE cipher suite');
        }
    }

    print("1. Testing x.509 auth to mongod");
    {
        const x509_options = {
            tlsMode: "preferTLS",
            tlsCAFile: CA_CERT,
            tlsPEMKeyFile: SERVER_CERT,
            ipv6: "",
            bind_ip_all: ""
        };
        let mongod = MongoRunner.runMongod(x509_options);
        const cipherDict = runSSLYze(mongod.port);
        if (cipherDict !== null) {
            testSSLYzeOutput(cipherDict);
        }
        MongoRunner.stopMongod(mongod);
    }
}());
