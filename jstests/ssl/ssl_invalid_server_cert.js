// Test SSL Certificate Expiration Monitoring
// This tests that a mongod with --sslMode requireSSL will not start with an
// X.509 certificate that is not yet valid or has expired.

// This test ensures that a mongod will not start with a certificate that is
// not yet valid. Tested certificate will become valid 06-17-2020.
var md = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/not_yet_valid.pem",
    sslCAFile: "jstests/libs/ca.pem"
});

assert.eq(null, md, "Possible to start mongod with not yet valid certificate.");

// This test ensures that a mongod with SSL will not start with an expired certificate.
md = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/expired.pem",
    sslCAFile: "jstests/libs/ca.pem"
});

assert.eq(null, md, "Possible to start mongod with expired certificate");
