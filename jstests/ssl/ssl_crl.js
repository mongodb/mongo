// Test CRLs
// This tests that using a CRL will allow clients with unrevoked certificates to connect.
// Also, tests that a server using an expired CRL will not allow connections.
// Note: crl_expired.pem is a CRL with no revoked certificates, but is an expired CRL.
//       crl.pem is a CRL with no revoked certificates.

import {TLSTest} from "jstests/libs/ssl_test.js";
import {requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

requireSSLProvider(["openssl", "windows"], function () {
    let testUnrevoked = new TLSTest(
        // Server option overrides
        {tlsMode: "requireTLS", tlsCRLFile: "jstests/libs/crl.pem"},
    );

    assert(testUnrevoked.connectWorked());

    let testRevoked = new TLSTest(
        // Server option overrides
        {tlsMode: "requireTLS", tlsCRLFile: "jstests/libs/crl_expired.pem"},
    );

    assert(!testRevoked.connectWorked());
});
