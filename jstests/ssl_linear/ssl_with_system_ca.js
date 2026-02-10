// On MacOS this test assumes that certificates exist at
// /opt/x509/macos-trusted-[ca|server|client].pem, and that /opt/x509/macos-trusted-ca.pem has
// been added as a trusted certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildhost-configuration/blob/1c1fcb51924cd4f1bc9eaf5db23f6e4365d6ba17/roles/macos/tasks/keychains.yml#L58-L87
// for details.
// To install certificates for local testing on OSX, invoke the following at a console:
//   mkdir /opt/x509
//   python x509/mkcert.py x509/apple_certs.json -o /opt/x509
//   security add-trusted-cert -d /opt/x509/macos-trusted-ca.pem
//   security add-trusted-cert -d -r trustAsRoot /opt/x509/macos-trusted-server.pem
//   security add-trusted-cert -d -r trustAsRoot /opt/x509/macos-trusted-client.pem

import {getPython3Binary} from "jstests/libs/python.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {copyCertificateFile} from "jstests/ssl/libs/ssl_helpers.js";

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
jsTest.log("HOST_TYPE = " + HOST_TYPE);

let trustedCA = getX509Path("trusted-ca.pem");
let trustedServer = getX509Path("trusted-server.pem");
let trustedClient = getX509Path("trusted-client.pem");

if (HOST_TYPE == "macOS") {
    trustedCA = "/opt/x509/macos-trusted-ca.pem";
    trustedServer = "/opt/x509/macos-trusted-server.pem";
    trustedClient = "/opt/x509/macos-trusted-client.pem";
    // Ensure trustedCA is properly installed on MacOS hosts.
    // (MacOS is the only OS where it is installed outside of this test)
    let exitCode = runProgram("security", "verify-cert", "-c", trustedClient);
    assert.eq(0, exitCode, "Check for proper installation of Trusted CA on MacOS host");
}
if (HOST_TYPE == "windows") {
    assert.eq(0, runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"));

    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", trustedCA);

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", trustedCA);
}

const certDir = MongoRunner.toRealDir("$dataDir/ssl_with_system_ca_test/");
if (HOST_TYPE == "linux") {
    mkdir(certDir);
    clearRawMongoProgramOutput();
    assert.eq(0, runProgram("openssl", "x509", "-hash", "-noout", "-in", trustedCA));
    let hash = rawMongoProgramOutput(".*");
    jsTestLog(hash); // has form: "|sh<pid> <hash>\n"
    hash = hash.trim().split(" ")[1];
    copyCertificateFile(trustedCA, `${certDir}/${hash}.0`);
}

// Tests server ingress validation works if the server is configured to use system CA.
function testServerIngress() {
    jsTestLog("Running testServerIngress");

    // Start a mongod configured with sslPEMKeyFile = trustedServer,
    // and a system CA store containing trustedCA.
    const serverOpts = {
        tlsMode: "preferTLS",
        tlsCertificateKeyFile: trustedServer,
        tlsAllowInvalidHostnames: "",
        waitForConnect: true,
        setParameter: {tlsUseSystemCA: true},
        env: {"SSL_CERT_DIR": certDir},
    };
    const conn = MongoRunner.runMongod(serverOpts);

    // Using trusted keys, the client should be able to connect.
    jsTestLog("Testing server ingress validates trusted client certificate");
    let clientOpts = {
        tls: {
            certificateKeyFile: trustedClient,
            CAFile: trustedCA,
            allowInvalidHostnames: true,
        },
    };
    assert.doesNotThrow(() => {
        let newConn = new Mongo(conn.host, undefined, clientOpts);
        assert.commandWorked(newConn.getDB("admin").runCommand({buildinfo: 1}));
    });

    // Using untrusted keys, verify the server rejects the client.
    jsTestLog("Testing server ingress rejects untrusted client certificate");
    clientOpts.tls.certificateKeyFile = getX509Path("client.pem");
    assert.commandWorked(conn.adminCommand({clearLog: "global"}));

    let error = assert.throwsWithCode(() => {
        new Mongo(conn.host, undefined, clientOpts);
    }, [ErrorCodes.SocketException, ErrorCodes.HostUnreachable]); // Different error depending on OS
    assert(error.reason.includes("handshake failed"), error.reason);
    checkLog.containsRelaxedJson(conn, 22988, {error: {code: ErrorCodes.SSLHandshakeFailed}});

    jsTestLog("Stopping mongod...");
    MongoRunner.stopMongod(conn);
}

// Tests server egress validation works if the server is configured to use system CA.
function testServerEgress() {
    jsTest.log("Running testServerEgress");

    // Start a replica set with one mongod configured with sslPEMKeyFile = trustedServer,
    // and a system CA store containing trustedCA.
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet({
        tlsMode: "preferTLS",
        tlsCertificateKeyFile: trustedServer, // used on ingress
        tlsClusterFile: trustedClient, // used on egress to node2
        tlsAllowInvalidHostnames: "",
        waitForConnect: true,
        setParameter: {tlsUseSystemCA: true},
        env: {"SSL_CERT_DIR": certDir},
    });
    rst.initiate();
    rst.awaitReplication();
    let conn = rst.getPrimary();

    // Add new node that uses a key not trusted by the first node.
    let badNode = rst.add({
        tlsMode: "preferTLS",
        tlsCertificateKeyFile: getX509Path("server.pem"), // used on ingress, untrusted
        tlsClusterFile: trustedClient, // used on egress to node1
        tlsCAFile: trustedCA,
        tlsAllowInvalidHostnames: "",
        waitForConnect: true,
    });

    assert.commandWorked(conn.adminCommand({clearLog: "global"}));
    jsTestLog("Reinitiating replica set with one additional node using untrusted key file");
    rst.reInitiate();

    // Verify node 1 can't connect to node 2
    checkLog.containsRelaxedJson(conn, 23722, {responseStatus: {code: ErrorCodes.HostUnreachable}});

    rst.remove(rst.getNodeId(badNode));

    // Add new node that uses a key trusted by the first node.
    let goodNode = rst.add({
        tlsMode: "preferTLS",
        tlsCertificateKeyFile: trustedServer, // used on ingress, trusted
        tlsClusterFile: trustedClient, // used on egress to node1
        tlsCAFile: trustedCA,
        tlsAllowInvalidHostnames: "",
        waitForConnect: true,
    });

    assert.commandWorked(conn.adminCommand({clearLog: "global"}));
    jsTestLog("Reinitiating replica set with one additional node using trusted key file");
    rst.reInitiate();
    rst.awaitSecondaryNodes();

    // Verify node 1 can now connect to node 2
    assert.commandWorked(conn.adminCommand({replSetTestEgress: 1}));

    jsTestLog("Stopping the replica set...");
    rst.stopSet();
}

try {
    testServerIngress();
    testServerEgress();
} finally {
    if (HOST_TYPE == "windows") {
        const trusted_ca_thumbprint = cat(getX509Path("trusted-ca.pem.digest.sha1"));
        runProgram("certutil.exe", "-delstore", "-f", "Root", trusted_ca_thumbprint);
        runProgram("certutil.exe", "-delstore", "-user", "-f", "CA", trusted_ca_thumbprint);
    }
}
