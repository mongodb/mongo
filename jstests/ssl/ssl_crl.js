// Test CRLs
// This tests that using a CRL will allow clients with unrevoked certificates to connect.
// Also, tests that a server using an expired CRL will not allow connections.
// Note: crl_expired.pem is a CRL with no revoked certificates, but is an expired CRL.
//       crl.pem is a CRL with no revoked certificates.

// This test should allow the user to connect with client.pem certificate.
ports = allocatePorts(2);
port1 = ports[0];
var baseName = "jstests_ssl_ssl_crl";


var md = startMongod("--port", port1, "--dbpath", 
                     "/data/db/" + baseName + "1", "--sslOnNormalPorts",
                     "--sslPEMKeyFile", "jstests/libs/server.pem",
                     "--sslCAFile", "jstests/libs/ca.pem",
                     "--sslCRLFile", "jstests/libs/crl.pem");


var mongo = runMongoProgram("mongo", "--port", port1, "--ssl", 
                            "--sslPEMKeyFile", "jstests/libs/client.pem",
                            "--eval", ";");

// 0 is the exit code for success
assert(mongo==0);

port2 = ports[1];
// This test ensures clients cannot connect if the CRL is expired.
md = startMongod("--port", port2, "--dbpath", "/data/db/" + baseName + "2", "--sslOnNormalPorts",
                 "--sslPEMKeyFile", "jstests/libs/server.pem",
                 "--sslCAFile", "jstests/libs/ca.pem",
                 "--sslCRLFile", "jstests/libs/crl_expired.pem");


mongo = runMongoProgram("mongo", "--port", port2, "--ssl", 
                        "--sslPEMKeyFile", "jstests/libs/client.pem",
                        "--eval", ";");

// 1 is the exit code for failure
assert(mongo==1);

