/**
 * Check that mongod will use the intracluster certificates for communication with mongot, if they
 * are available.
 */
import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {
    CA_CERT,
    SERVER_CERT,
    TRUSTED_CA_CERT,
    TRUSTED_SERVER_CERT
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
    tlsAllowInvalidCertificates: ""
});

const db = conn.getDB("test");
const coll = db.getCollection("search");
const pipeline = [{$search: {query: "cakes", path: "title"}}];
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

// Perform a $search query and assert that the connection fails due to invalid certificates.
// We cannot assert on a specific error message because it will vary based on the transport
// protocol used.
assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.HostUnreachable, "");

MongoRunner.stopMongod(conn);
mongotmock.stop();
