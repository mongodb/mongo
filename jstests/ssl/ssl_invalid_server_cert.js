// Test SSL Certificate Expiration Monitoring
// This tests that a merizod with --sslMode requireSSL will not start with an
// X.509 certificate that is not yet valid or has expired.

// This test ensures that a merizod will not start with a certificate that is
// not yet valid. Tested certificate will become valid 06-17-2020.
var md = MerizoRunner.runMerizod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/not_yet_valid.pem",
    sslCAFile: "jstests/libs/ca.pem"
});

assert.eq(null, md, "Possible to start merizod with not yet valid certificate.");

// This test ensures that a merizod with SSL will not start with an expired certificate.
md = MerizoRunner.runMerizod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/expired.pem",
    sslCAFile: "jstests/libs/ca.pem"
});

assert.eq(null, md, "Possible to start merizod with expired certificate");
