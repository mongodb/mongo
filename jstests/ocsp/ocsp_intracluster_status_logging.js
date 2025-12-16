// Tests that OCSP for intracluster TLS logs helpful error messages when verification
// fails due to revoked peer certificates.
// @tags: [requires_http_client, requires_ocsp_stapling]

import {FAULT_REVOKED, MockOCSPServer} from "jstests/ocsp/lib/mock_ocsp.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {
    OCSP_CA_PEM,
    OCSP_CLIENT_CERT,
    OCSP_SERVER_CERT_REVOKED,
    supportsStapling,
    waitForServer,
    assertClientConnectFails,
} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
}

const repl_set = "ocsp-test";
const with_stapling_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: OCSP_SERVER_CERT_REVOKED,
    tlsCAFile: OCSP_CA_PEM,
    tlsAllowInvalidHostnames: "",
    setParameter: {
        "ocspEnabled": "true",
    },
    replSet: repl_set,
    waitForConnect: false,
};
const no_stapling_options = Object.assign({}, with_stapling_options);
no_stapling_options.setParameter = {
    "ocspEnabled": "true",
    "failpoint.disableStapling": "{'mode':'alwaysOn'}",
};

const revoked_status = {
    "code": ErrorCodes.OCSPCertificateStatusRevoked,
    "codeName": "OCSPCertificateStatusRevoked",
    "errmsg": "OCSP Certificate Status: Revoked. Reason: unspecified",
};

const mock_ocsp = new MockOCSPServer(FAULT_REVOKED, 1);
mock_ocsp.start();

// Start the replica set nodes:
// - conn1 does not staple
// - conn2 staples revoked status,
let conn1 = MongoRunner.runMongod(Object.assign({}, no_stapling_options));
let conn2 = MongoRunner.runMongod(Object.assign({}, with_stapling_options));
waitForServer(conn1);
waitForServer(conn2);

// Assert the current shell can't connect because of the revoked server certs.
assertClientConnectFails(conn1);
assertClientConnectFails(conn2);

// In a separate shell (that ignores OCSP), initiate the replica set from conn1.
// Expect conn1 connects to conn2; fails because conn2's stapled cert is revoked.
async function bgValidateFunc(rs_name, ports, has_stapling_logs, expected_logid, expected_attr) {
    function waitForLog(code, attr) {
        assert.soon(
            function () {
                return checkLog.checkContainsWithAtLeastCountJson(db, code, attr, 1, "I", true);
            },
            `Could not find log entries containing id ${code} and attr: ${tojson(attr)}`,
            undefined,
            undefined,
            {runHangAnalyzer: false},
        );
    }

    // If this node staples, assert there are logs for both fetch/staple status AND
    // the certificate revocation status.
    if (has_stapling_logs) {
        waitForLog(577163 /* OCSP fetch status */, {"status": {"code": 0}});
        waitForLog(8464500 /* OCSP revocation status */, {
            "status": {"code": ErrorCodes.OCSPCertificateStatusRevoked},
        });
    } else {
        assert.eq(checkLog.checkContainsOnceJson(db, 577163, {}), false);
    }

    // Initiate the replica set to start egress connection to the other node.
    assert.eq(checkLog.checkContainsOnceJson(db, expected_logid, {}), false);
    rs.initiate({
        _id: rs_name,
        members: ports.map((port, idx) => {
            return {_id: idx, host: `localhost:${port}`};
        }),
    });
    waitForLog(expected_logid, expected_attr);
}

let pshell = startParallelShell(
    funWithArgs(bgValidateFunc, repl_set, [conn1.port, conn2.port], false, 23225, {"error": revoked_status}),
    conn1.port,
    false,
    "--tls",
    "--tlsCAFile",
    OCSP_CA_PEM,
    "--tlsCertificateKeyFile",
    OCSP_CLIENT_CERT,
    "--tlsAllowInvalidHostnames",
    "--tlsAllowInvalidCertificates",
    "--verbose",
    1,
);
pshell();

// Use a parallel shell to initiate the replica set from conn2.
// Expect conn2 connects to conn1, does OCSP on-demand status checking,
// and fails because peer cert is revoked.
pshell = startParallelShell(
    funWithArgs(bgValidateFunc, repl_set, [conn1.port, conn2.port], true, 8464502, {"status": revoked_status}),
    conn2.port,
    false,
    "--tls",
    "--tlsCAFile",
    OCSP_CA_PEM,
    "--tlsCertificateKeyFile",
    OCSP_CLIENT_CERT,
    "--tlsAllowInvalidHostnames",
    "--tlsAllowInvalidCertificates",
    "--verbose",
    1,
);
pshell();

MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);

mock_ocsp.stop();
