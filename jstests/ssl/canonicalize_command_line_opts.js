// Ensure that all 'ssl' options are canonicalized to their modern 'tls' versions.

(function() {
'use strict';

function runTest(mongod) {
    assert(mongod);
    const admin = mongod.getDB('admin');

    const opts = assert.commandWorked(admin.runCommand({getCmdLineOpts: 1}));
    print(tojson(opts));
    assert.eq(typeof (opts), 'object');
    assert.eq(typeof (opts.parsed), 'object');
    assert.eq(typeof (opts.parsed.net), 'object');

    const net = opts.parsed.net;
    assert.eq(typeof (net.ssl), 'undefined');
    assert.eq(typeof (net.tls), 'object');

    const tls = net.tls;
    assert.eq(tls.mode, 'requireTLS');
    assert.eq(tls.CAFile, 'jstests/libs/ca.pem');
    assert.eq(tls.certificateKeyFile, 'jstests/libs/server.pem');
    assert.eq(tls.allowConnectionsWithoutCertificates, true);
    assert.eq(tls.allowInvalidHostnames, true);
}

const options = {
    sslMode: 'requireSSL',
    sslCAFile: 'jstests/libs/ca.pem',
    sslPEMKeyFile: 'jstests/libs/server.pem',
    sslAllowConnectionsWithoutCertificates: '',
    sslAllowInvalidHostnames: '',
};

const mongod = MongoRunner.runMongod(options);
runTest(mongod);
MongoRunner.stopMongod(mongod);
})();
