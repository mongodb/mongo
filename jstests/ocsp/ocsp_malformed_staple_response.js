/**
 * Tests that mongod does not crash when an outbound (egress) TLS connection receives a
 * malformed stapled OCSP response from its peer.
 *
 * Uses a peer that staples an undecodable OCSP response, which used to make mongod's client-side
 * OCSP callback dereference the nullptr returned by d2i_OCSP_RESPONSE(), crashing the
 * process. The fix rejects the response (treating the staple as not acceptable) and
 * keeps the server running. Here we stand up a malicious TLS server that staples a
 * malformed response, point a replica set heartbeat at it to drive mongod's egress TLS
 * path, and assert mongod logs the rejection and stays alive.
 *
 * @tags: [requires_http_client, requires_ocsp_stapling]
 */
import {MockMalformedStapleServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {OCSP_CA_PEM, OCSP_SERVER_CERT, supportsStapling} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
}

// The log id emitted by ocspClientCallback when it rejects a stapled response that cannot
// be decoded from DER. Its presence proves the malformed staple reached the (fixed) code
// path rather than crashing the server.
const kMalformedStapleLogId = 12836200;

// Note: we intentionally do NOT set tlsAllowInvalidCertificates. With invalid certificates
// allowed, the egress OCSP callback short-circuits before it ever decodes the stapled
// response, so it would not exercise the fix.
const ocspOptions = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: OCSP_SERVER_CERT,
    tlsCAFile: OCSP_CA_PEM,
    tlsAllowInvalidHostnames: "",
    setParameter: {
        "ocspEnabled": "true",
    },
    replSet: jsTestName(),
};

// Reserve a port for the malicious TLS server and start it. It presents the OCSP server
// certificate (which carries an OCSP AIA URI, so mongod will run its stapled-response
// handling) and staples a malformed (single 0x00 byte) OCSP response on every handshake.
const maliciousPort = allocatePort();
const maliciousServer = new MockMalformedStapleServer(maliciousPort);
maliciousServer.start();

const conn = MongoRunner.runMongod(ocspOptions);

try {
    // Bring up a single-node replica set so we have a primary that sends heartbeats.
    assert.commandWorked(
        conn.adminCommand({replSetInitiate: {_id: jsTestName(), members: [{_id: 0, host: conn.host}]}}),
    );
    assert.soon(() => conn.adminCommand({hello: 1}).isWritablePrimary, "node never became primary");

    // Add the malicious server as a non-voting, hidden member. This keeps the existing node
    // as primary (voting majority is unchanged) while causing it to repeatedly heartbeat the
    // malicious member, driving mongod's egress TLS path that receives the malformed staple.
    // The member host must use the same hostname style as the existing node, since a replica
    // set config may not mix localhost and non-localhost host names.
    const config = assert.commandWorked(conn.adminCommand({replSetGetConfig: 1})).config;
    const hostName = config.members[0].host.split(":")[0];
    config.version++;
    config.members.push({
        _id: 1,
        host: hostName + ":" + maliciousPort,
        priority: 0,
        votes: 0,
        hidden: true,
    });
    assert.commandWorked(conn.adminCommand({replSetReconfig: config}));

    // The heartbeats now hit the malicious server. Before the fix, the first heartbeat that
    // received the malformed staple crashed mongod. After the fix, mongod logs the rejection
    // and keeps running. Heartbeats retry on an interval, so this log appears reliably.
    assert.soon(
        () => checkLog.checkContainsOnceJson(conn, kMalformedStapleLogId, {}),
        "mongod did not log rejection of the malformed stapled OCSP response",
        undefined,
        undefined,
        {runHangAnalyzer: false},
    );

    // Ensure the malformed staple was handled gracefully and mongod is still
    // alive and serving commands.
    assert(checkProgram(conn.pid).alive, "mongod process is no longer running");
    assert.commandWorked(conn.adminCommand({ping: 1}), "mongod stopped responding to commands");
} finally {
    MongoRunner.stopMongod(conn);
    // The mongoRunner can spawn a validation shell that races with the responder shutdown on
    // some platforms; mirror the small settling sleep used by the other OCSP tests.
    sleep(1000);
    maliciousServer.stop();
}
