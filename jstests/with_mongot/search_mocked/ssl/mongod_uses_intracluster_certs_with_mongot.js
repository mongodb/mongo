/**
 * Check that mongod will use the intracluster certificates for communication with mongot, if they
 * are available.
 */
import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {
    CA_CERT,
    SERVER_CERT,
    TRUSTED_CA_CERT,
    TRUSTED_SERVER_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start({tlsMode: "requireTLS"});
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({
    sslMode: "requireSSL",
    setParameter: {mongotHost: mongotConn.host},
    sslCAFile: CA_CERT,
    sslPEMKeyFile: SERVER_CERT,
    // These will be invalid if they are used to communicate with mongot.
    tlsClusterCAFile: TRUSTED_CA_CERT,
    tlsClusterFile: TRUSTED_SERVER_CERT,
    tlsAllowInvalidCertificates: "",
});

const db = conn.getDB("test");
const coll = db.getCollection("search");
const pipeline = [{$search: {query: "cakes", path: "title"}}];
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

// Perform a $search query and assert the connection fails due to invalid certificates: mongod
// presents its intracluster cert to mongot, which doesn't trust it and tears the connection down
// during the TLS handshake. Whether the peer-close is a graceful FIN or an abortive RST, the egress
// client now classifies it as ConnectionClosedByPeer; a lower-level socket failure can surface as
// SocketException, and a failed connect as HostUnreachable. We accept any of the three.
assertErrCodeAndErrMsgContains(
    coll,
    pipeline,
    [ErrorCodes.SocketException, ErrorCodes.HostUnreachable, ErrorCodes.ConnectionClosedByPeer],
    "",
);

MongoRunner.stopMongod(conn);
mongotmock.stop();
