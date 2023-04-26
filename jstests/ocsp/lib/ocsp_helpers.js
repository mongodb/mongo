/**
 * Helper variables and methods for OCSP
 */

load("jstests/ssl/libs/ssl_helpers.js");

const OCSP_CA_PEM = "jstests/libs/ocsp/ca_ocsp.pem";
const OCSP_CA_CERT = "jstests/libs/ocsp/ca_ocsp.crt";
const OCSP_CA_KEY = "jstests/libs/ocsp/ca_ocsp.key";
const CLUSTER_CA_CERT = "jstests/libs/ca.pem";
const CLUSTER_KEY = "jstests/libs/server.pem";
const OCSP_SERVER_CERT = "jstests/libs/ocsp/server_ocsp.pem";
const OCSP_CLIENT_CERT = "jstests/libs/ocsp/client_ocsp.pem";
const OCSP_SERVER_MUSTSTAPLE_CERT = "jstests/libs/ocsp/server_ocsp_mustStaple.pem";
const OCSP_SERVER_CERT_REVOKED = "jstests/libs/ocsp/server_ocsp_revoked.pem";
const OCSP_RESPONDER_CERT = "jstests/libs/ocsp/ocsp_responder.crt";
const OCSP_RESPONDER_KEY = "jstests/libs/ocsp/ocsp_responder.key";
const OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM = "jstests/libs/ocsp/intermediate_ca_with_root_ocsp.pem";
const OCSP_INTERMEDIATE_CA_ONLY_CERT = "jstests/libs/ocsp/intermediate_ca_only_ocsp.crt";
const OCSP_INTERMEDIATE_CA_ONLY_KEY = "jstests/libs/ocsp/intermediate_ca_only_ocsp.key";
const OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM =
    "jstests/libs/ocsp/server_signed_by_intermediate_ca_ocsp.pem";
const OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM =
    "jstests/libs/ocsp/server_and_intermediate_ca_appended_ocsp.pem";

var clearOCSPCache = function() {
    let provider = determineSSLProvider();
    if (provider === "apple") {
        runMongoProgram("find",
                        "/private/var/folders/cl/",
                        "-regex",
                        "'.*\/C\/com.apple.trustd\/ocspcache.sqlite.*'",
                        "-delete");
    } else if (provider === "windows") {
        runMongoProgram("certutil", "-urlcache", "*", "delete");
    }
};

var waitForServer = function(conn) {
    const host = "localhost:" + conn.port;
    const provider = determineSSLProvider();

    if (provider !== "windows") {
        assert.soon(() => {
            return 0 ===
                runMongoProgram('./mongo',
                                '--host',
                                host,
                                '--tls',
                                '--tlsCAFile',
                                OCSP_CA_PEM,
                                '--tlsCertificateKeyFile',
                                OCSP_CLIENT_CERT,
                                '--tlsAllowInvalidCertificates',
                                '--tlsAllowInvalidHostnames',
                                '--eval',
                                '";"');
        });
    } else {
        sleep(15000);
    }
};

var clientConnect = function(conn) {
    const exitCode = runMongoProgram("mongo",
                                     "--host",
                                     "localhost",
                                     "--port",
                                     conn.port,
                                     "--tls",
                                     "--tlsCAFile",
                                     OCSP_CA_PEM,
                                     "--tlsCertificateKeyFile",
                                     OCSP_CLIENT_CERT,
                                     "--tlsAllowInvalidHostnames",
                                     "--verbose",
                                     1,
                                     "--eval",
                                     ";");
    return exitCode;
};

const OCSP_REVOKED = "OCSPCertificateStatusRevoked";

var assertClientConnectFails = function(conn, reason) {
    clearRawMongoProgramOutput();
    assert.neq(clientConnect(conn), 0);
    const errmsg = rawMongoProgramOutput();
    if (typeof reason === 'string' || reason instanceof RegExp) {
        assert.neq(errmsg.search(reason), -1);
    }
};

var assertClientConnectSucceeds = function(conn) {
    assert.eq(clientConnect(conn), 0);
};

var supportsStapling = function() {
    if (determineSSLProvider() !== "openssl") {
        return false;
    }
    if (isUbuntu1804() === true) {
        return false;
    }
    return true;
};
