// Test CRLs
// This tests that using a CRL will allow clients with unrevoked certificates to connect.
// Also, tests that a server using an expired CRL will not allow connections.
// Note: crl_expired.pem is a CRL with no revoked certificates, but is an expired CRL.
//       crl.pem is a CRL with no revoked certificates.

load("jstests/libs/ssl_test.js");

var testUnrevoked = new SSLTest(
    // Server option overrides
    {sslMode: "requireSSL", sslCRLFile: "jstests/libs/crl.pem"});

assert(testUnrevoked.connectWorked());

var testRevoked = new SSLTest(
    // Server option overrides
    {sslMode: "requireSSL", sslCRLFile: "jstests/libs/crl_expired.pem"});

assert(!testRevoked.connectWorked());
