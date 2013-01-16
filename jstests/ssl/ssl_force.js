// Test forcing certificate validation
// This tests that forcing certification validation will prohibit clients without certificates
// from connecting.
port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_ssl_ssl_force";


var md = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--sslOnNormalPorts",
                      "--sslPEMKeyFile", "jstests/libs/server.pem",
                      "--sslCAFile", "jstests/libs/ca.pem",
                      "--sslForceCertificateValidation");


var mongo = runMongoProgram("mongo", "--port", port, "--ssl", 
                            "--eval", ";");

// 1 is the exit code for failure
assert(mongo==1);


var mongo = runMongoProgram("mongo", "--port", port, "--ssl", 
                            "--sslPEMKeyFile", "jstests/libs/client.pem",
                            "--eval", ";");

// 0 is the exit code for success
assert(mongo==0);
