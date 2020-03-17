/**
 * Helper variables and methods for OCSP
 */

load("jstests/ssl/libs/ssl_helpers.js");

const OCSP_CA_CERT = "jstests/libs/ocsp/ca_ocsp.pem";
const OCSP_SERVER_CERT = "jstests/libs/ocsp/server_ocsp.pem";
const OCSP_CLIENT_CERT = "jstests/libs/ocsp/client_ocsp.pem";
const OCSP_SERVER_MUSTSTAPLE_CERT = "jstests/libs/ocsp/server_ocsp_mustStaple.pem";
const OCSP_RESPONDER_CERT = "jstests/libs/ocsp/ocsp_responder.crt";
const OCSP_RESPONDER_KEY = "jstests/libs/ocsp/ocsp_responder.key";
