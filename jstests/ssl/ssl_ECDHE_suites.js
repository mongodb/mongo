// Test that a client can authenicate against the server with roles.
// Also validates RFC2253
load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

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
            let cipherDict = JSON.parse(ciphers);
            return cipherDict;
        } catch (e) {
            jsTestLog("Failed to parse: " + ciphers + "\n" + ciphers);
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
        for (var i = 0; i < 3; i++) {
            assert.eq(cipherDict[suites[i]].length, 0);
        }

        // Printing TLS 1.1, 1.2, and 1.3 suites that are accepted
        for (var i = 3; i < 6; i++) {
            const TLSVersion = cipherDict[suites[i]].toString().split(",");
            print('*************************\n' + suites[i] + ": ");
            for (var j = 0; j < TLSVersion.length; j++) {
                print(TLSVersion[j]);
            }
        }
    }

    print("1. Testing x.509 auth to mongod");
    {
        const x509_options = {
            sslMode: "requireSSL",
            sslCAFile: CA_CERT,
            sslPEMKeyFile: SERVER_CERT,
            ipv6: "",
            bind_ip_all: ""
        };
        let mongod = MongoRunner.runMongod(x509_options);
        var cipherDict = runSSLYze(mongod.port);
        if (cipherDict !== null) {
            testSSLYzeOutput(cipherDict);
        }
        MongoRunner.stopMongod(mongod);
    }
}());