// Test CRLs
// This tests that using a CRL will allow clients with unrevoked certificates to connect.
// Note: crl.pem is a CRL with no revoked certificates.
// This test should allow the user to connect with client.pem certificate.
port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_ssl_ssl_crl";


var md = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--sslOnNormalPorts",
                      "--sslPEMKeyFile", "jstests/libs/server.pem",
                      "--sslCAFile", "jstests/libs/ca.pem",
                      "--sslCRLFile", "jstests/libs/crl.pem");


var mongo = runMongoProgram("mongo", "--port", port, "--ssl", 
                            "--sslPEMKeyFile", "jstests/libs/client.pem",
                            "--eval", ";");

// 0 is the exit code for success
assert(mongo==0);


