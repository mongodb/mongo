// Test changing the --sslMode and --clusterAuthMode parameters using setParameter

import {ReplSetTest} from "jstests/libs/replsettest.js";

let SERVER_CERT = "jstests/libs/server.pem";
let CA_CERT = "jstests/libs/ca.pem";

class TransportMode {
    constructor(sslName, tlsName) {
        this.sslName = sslName;
        this.tlsName = tlsName;
    }

    get sslMode() {
        return this.sslName;
    }

    get tlsMode() {
        return this.tlsName;
    }
}

const invalid = new TransportMode("invalid", "invalid");
const disabled = new TransportMode("disabled", "disabled");
const allowed = new TransportMode("allowSSL", "allowTLS");
const prefered = new TransportMode("preferSSL", "preferTLS");
const required = new TransportMode("requireSSL", "requireTLS");

function testTransportTransitionStandalone(scheme, oldMode, newMode, shouldSucceed) {
    let conn = MongoRunner.runMongod({sslMode: oldMode, sslPEMKeyFile: SERVER_CERT, sslCAFile: CA_CERT});

    let adminDB = conn.getDB("admin");
    adminDB.createUser({user: "root", pwd: "pwd", roles: ["root"]});
    adminDB.auth("root", "pwd");
    let res = adminDB.runCommand({"setParameter": 1, [scheme]: newMode[scheme]});

    assert(res["ok"] == shouldSucceed, tojson(res));
    if (!shouldSucceed) {
        MongoRunner.stopMongod(conn);
        return;
    }

    if (newMode != "requireSSL") {
        MongoRunner.stopMongod(conn);
        return;
    }

    let uri = `mongodb://localhost:${conn.port}/admin`;
    let exitCode = runMongoProgram("mongo", uri, "--eval", "assert.commandWorked(db.hello())");
    assert.neq(exitCode, 0, "Was able to connect without SSL when SSLMode was requireSSL");
    MongoRunner.stopMongod(conn);
}

function testTransportTransitionCluster(scheme, oldMode, newMode) {
    let rst = new ReplSetTest({
        name: "switch",
        nodes: 3,
        nodeOptions: {
            sslMode: oldMode,
            sslAllowInvalidHostnames: "",
            sslCAFile: CA_CERT,
            sslPEMKeyFile: SERVER_CERT,
        },
    });

    rst.startSet();
    rst.initiate();
    rst.awaitReplication();

    print(`=== Switching ${scheme} from ${oldMode} to ${newMode[scheme]} for all nodes in cluster`);
    for (let n of rst.nodes) {
        let adminDB = n.getDB("admin");
        assert.commandWorked(adminDB.runCommand({"setParameter": 1, [scheme]: newMode[scheme]}));
    }
    rst.awaitReplication();

    rst.stopSet();
}

function testTransportTransition(scheme, oldMode, newMode, shouldSucceed) {
    testTransportTransitionStandalone(scheme, oldMode, newMode, shouldSucceed);
    if (shouldSucceed) {
        testTransportTransitionCluster(scheme, oldMode, newMode);
    }
}

function testAuthModeTransition(oldMode, newMode, sslMode, shouldSucceed) {
    const keyFile = "jstests/libs/key1";

    let config = {
        sslMode: sslMode,
        sslPEMKeyFile: SERVER_CERT,
        sslCAFile: CA_CERT,
        clusterAuthMode: oldMode,
    };

    if (oldMode != "x509") {
        config.keyFile = keyFile;
    }

    const conn = MongoRunner.runMongod(config);
    const adminDB = conn.getDB("admin");
    let authAsKeyFileCluster = function () {
        const authParams = {
            user: "__system",
            mechanism: "SCRAM-SHA-256",
            pwd: cat(keyFile).replace(/[\011-\015\040]/g, ""),
        };

        return adminDB.auth(authParams);
    };

    if (oldMode != "x509") {
        assert(authAsKeyFileCluster());
    }

    let res = adminDB.runCommand({"setParameter": 1, "clusterAuthMode": newMode});
    assert(res["ok"] == shouldSucceed, tojson(res));

    if (shouldSucceed && oldMode != "x509") {
        if (newMode == "x509") {
            assert(!authAsKeyFileCluster(), "Key file cluster auth should no longer work");
        } else {
            assert(authAsKeyFileCluster(), "Key file cluster auth should still work");
        }
    }

    MongoRunner.stopMongod(conn);
}

function testTransportTransitions(scheme) {
    testTransportTransition(scheme, "allowSSL", invalid, false);
    testTransportTransition(scheme, "allowSSL", disabled, false);
    testTransportTransition(scheme, "allowSSL", allowed, false);
    testTransportTransition(scheme, "allowSSL", prefered, true);
    testTransportTransition(scheme, "allowSSL", required, false);
    testTransportTransition(scheme, "preferSSL", invalid, false);
    testTransportTransition(scheme, "preferSSL", disabled, false);
    testTransportTransition(scheme, "preferSSL", allowed, false);
    testTransportTransition(scheme, "preferSSL", prefered, false);
    testTransportTransition(scheme, "preferSSL", required, true);
    testTransportTransition(scheme, "requireSSL", invalid, false);
    testTransportTransition(scheme, "requireSSL", disabled, false);
    testTransportTransition(scheme, "requireSSL", allowed, false);
    testTransportTransition(scheme, "requireSSL", prefered, false);
    testTransportTransition(scheme, "requireSSL", required, false);
}

testTransportTransitions("sslMode");
testTransportTransitions("tlsMode");

testAuthModeTransition("sendKeyFile", "invalid", "requireSSL", false);
testAuthModeTransition("sendKeyFile", "keyFile", "requireSSL", false);
testAuthModeTransition("sendKeyFile", "sendKeyFile", "requireSSL", false);
testAuthModeTransition("sendKeyFile", "sendX509", "requireSSL", true);
testAuthModeTransition("sendKeyFile", "x509", "requireSSL", false);
testAuthModeTransition("sendX509", "invalid", "requireSSL", false);
testAuthModeTransition("sendX509", "keyFile", "requireSSL", false);
testAuthModeTransition("sendX509", "sendKeyFile", "requireSSL", false);
testAuthModeTransition("sendX509", "sendX509", "requireSSL", false);
testAuthModeTransition("sendX509", "x509", "requireSSL", true);
testAuthModeTransition("x509", "invalid", "requireSSL", false);
testAuthModeTransition("x509", "keyFile", "requireSSL", false);
testAuthModeTransition("x509", "sendKeyFile", "requireSSL", false);
testAuthModeTransition("x509", "sendX509", "requireSSL", false);
testAuthModeTransition("x509", "x509", "requireSSL", false);

testAuthModeTransition("sendKeyFile", "invalid", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "keyFile", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "sendKeyFile", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "sendX509", "allowSSL", false);
testAuthModeTransition("sendKeyFile", "x509", "allowSSL", false);
