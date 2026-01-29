// Ensure that all 'ssl' options are canonicalized to their modern 'tls' versions.

function runTest(mongod) {
    assert(mongod);
    const admin = mongod.getDB("admin");

    const opts = assert.commandWorked(admin.runCommand({getCmdLineOpts: 1}));
    print(tojson(opts));
    assert.eq(typeof opts, "object");
    assert.eq(typeof opts.parsed, "object");
    assert.eq(typeof opts.parsed.net, "object");

    const net = opts.parsed.net;
    assert.eq(typeof net.ssl, "undefined");
    assert.eq(typeof net.tls, "object");

    const tls = net.tls;
    assert.eq(tls.mode, "requireTLS");
    assert.eq(tls.CAFile, getX509Path("ca.pem"));
    assert.eq(tls.certificateKeyFile, getX509Path("server.pem"));
    assert.eq(tls.allowConnectionsWithoutCertificates, true);
    assert.eq(tls.allowInvalidHostnames, true);
}

const options = {
    sslMode: "requireSSL",
    sslCAFile: getX509Path("ca.pem"),
    sslPEMKeyFile: getX509Path("server.pem"),
    sslAllowConnectionsWithoutCertificates: "",
    sslAllowInvalidHostnames: "",
};

const mongod = MongoRunner.runMongod(options);
runTest(mongod);
MongoRunner.stopMongod(mongod);
