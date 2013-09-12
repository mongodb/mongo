// Test a revoked CRL -- ensure that a connection is not allowed.
// Note: crl_client_revoked.pem is a CRL with the client.pem certificate listed as revoked.
// This test should test that the user cannot connect with client.pem certificate.

port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_ssl_ssl_crl_revoked";


var md = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--sslOnNormalPorts",
                    "--sslPEMKeyFile", "jstests/libs/server.pem",
                    "--sslCAFile", "jstests/libs/ca.pem",
                    "--sslCRLFile", "jstests/libs/crl_client_revoked.pem");


var mongo = runMongoProgram("mongo", "--port", port, "--ssl", 
                            "--sslPEMKeyFile", "jstests/libs/client_revoked.pem",
                            "--eval", ";");

// 1 is the exit code for the shell failing to connect, which is what we want
// for a successful test.
assert(mongo==1);

