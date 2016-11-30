(function() {
    'use strict';
    if (_isWindows()) {
        runProgram(
            "certutil.exe", "-addstore", "-user", "-f", "CA", "jstests\\libs\\trusted-ca.pem");
    }

    var replTest = new ReplSetTest({
        name: "ssltest",
        nodes: 1,
        nodeOptions: {
            sslMode: "requireSSL",
            sslPEMKeyFile: "jstests/libs/trusted-server.pem",
        },
        host: "localhost",
        useHostName: false,
    });

    replTest.startSet({
        env: {
            SSL_CERT_FILE: 'jstests/libs/trusted-ca.pem',
        },
    });
    replTest.initiate();

    var nodeList = replTest.nodeList().join();

    var checkShellOkay = function(url) {
        // Should not be able to authenticate with x509.
        // Authenticate call will return 1 on success, 0 on error.
        var argv = ['./mongo', url, '--eval', ('db.runCommand({replSetGetStatus: 1})')];
        if (!_isWindows()) {
            // On Linux we override the default path to the system CA store to point to our
            // "trusted" CA. On Windows, this CA will have been added to the user's trusted CA list
            argv.unshift("env", "SSL_CERT_FILE=jstests/libs/trusted-ca.pem");
        }
        return runMongoProgram(...argv);
    };

    var noMentionSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}`;
    jsTestLog(`Replica set url (doesn't mention SSL): ${noMentionSSLURL}`);
    assert.neq(checkShellOkay(noMentionSSLURL), 0, "shell correctly failed to connect without SSL");

    var useSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}&ssl=true`;
    jsTestLog(`Replica set url (uses SSL): ${useSSLURL}`);
    assert.eq(checkShellOkay(useSSLURL), 0, "successfully connected with SSL");

    var disableSSLURL = `mongodb://${nodeList}/admin?replicaSet=${replTest.name}&ssl=false`;
    jsTestLog(`Replica set url (doesnt use SSL): ${disableSSLURL}`);
    assert.neq(checkShellOkay(disableSSLURL), 0, "shell correctly failed to connect without SSL");
})();
