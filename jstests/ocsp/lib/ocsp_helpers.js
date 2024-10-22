/**
 * Helper variables and methods for OCSP
 */

import {isUbuntu1804} from "jstests/libs/os_helpers.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

export const OCSP_CA_PEM = "jstests/libs/ocsp/ca_ocsp.pem";
export const OCSP_CA_CERT = "jstests/libs/ocsp/ca_ocsp.crt";
export const OCSP_CA_KEY = "jstests/libs/ocsp/ca_ocsp.key";
export const CLUSTER_CA_CERT = "jstests/libs/ca.pem";
export const CLUSTER_KEY = "jstests/libs/server.pem";
export const OCSP_SERVER_CERT = "jstests/libs/ocsp/server_ocsp.pem";
export const OCSP_CLIENT_CERT = "jstests/libs/ocsp/client_ocsp.pem";
export const OCSP_SERVER_MUSTSTAPLE_CERT = "jstests/libs/ocsp/server_ocsp_mustStaple.pem";
export const OCSP_SERVER_CERT_REVOKED = "jstests/libs/ocsp/server_ocsp_revoked.pem";
export const OCSP_SERVER_CERT_INVALID = "jstests/libs/ocsp/server_ocsp_invalid.pem";
export const OCSP_RESPONDER_CERT = "jstests/libs/ocsp/ocsp_responder.crt";
export const OCSP_RESPONDER_KEY = "jstests/libs/ocsp/ocsp_responder.key";
export const OCSP_INTERMEDIATE_CA_WITH_ROOT_PEM =
    "jstests/libs/ocsp/intermediate_ca_with_root_ocsp.pem";
export const OCSP_INTERMEDIATE_CA_ONLY_CERT = "jstests/libs/ocsp/intermediate_ca_only_ocsp.crt";
export const OCSP_INTERMEDIATE_CA_ONLY_KEY = "jstests/libs/ocsp/intermediate_ca_only_ocsp.key";

export const OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM =
    "jstests/libs/ocsp/server_signed_by_intermediate_ca_ocsp.pem";

export const OCSP_SERVER_AND_INTERMEDIATE_APPENDED_PEM =
    "jstests/libs/ocsp/server_and_intermediate_ca_appended_ocsp.pem";

export var clearOCSPCache = function() {
    let provider = determineSSLProvider();
    if (provider === "apple") {
        runNonMongoProgram("find",
                           "/private/var/folders/cl/",
                           "-regex",
                           "'.*\/C\/com.apple.trustd\/ocspcache.sqlite.*'",
                           "-delete");
    } else if (provider === "windows") {
        runNonMongoProgram("certutil", "-urlcache", "*", "delete");
    }
};

export var waitForServer = function(conn) {
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

export var clientConnect = function(conn) {
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

export const OCSP_REVOKED = "OCSPCertificateStatusRevoked";

export var assertClientConnectFails = function(conn, reason) {
    clearRawMongoProgramOutput();
    assert.neq(clientConnect(conn), 0);
    const errmsg = rawMongoProgramOutput(".*");
    if (typeof reason === 'string' || reason instanceof RegExp) {
        assert.neq(errmsg.search(reason), -1);
    }
};

export var assertClientConnectSucceeds = function(conn) {
    assert.eq(clientConnect(conn), 0);
};

export var supportsStapling = function() {
    if (determineSSLProvider() !== "openssl") {
        return false;
    }
    if (isUbuntu1804() === true) {
        return false;
    }
    return true;
};
