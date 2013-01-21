// Test forcing certificate validation
// This tests that forcing certification validation will prohibit clients without certificates
// from connecting.
ports = allocatePorts( 2 );

var baseName = "jstests_ssl_ssl_weak";


// Test that connecting with no client certificate and --sslWeakCertificateValidation connects
// successfully.
var md = startMongod( "--port", ports[0], "--dbpath", "/data/db/" + baseName + "1", "--sslOnNormalPorts",
                      "--sslPEMKeyFile", "jstests/libs/server.pem",
                      "--sslCAFile", "jstests/libs/ca.pem",
                      "--sslWeakCertificateValidation");

var mongo = runMongoProgram("mongo", "--port", ports[0], "--ssl", 
                            "--eval", ";");

// 0 is the exit code for success
assert(mongo==0);


// Test that connecting with a valid client certificate connects successfully.
mongo = runMongoProgram("mongo", "--port", ports[0], "--ssl", 
                        "--sslPEMKeyFile", "jstests/libs/client.pem",
                        "--eval", ";");

// 0 is the exit code for success
assert(mongo==0);


// Test that connecting with no client certificate and no --sslWeakCertificateValidation fails to
// connect.
var md2 = startMongod( "--port", ports[1], "--dbpath", "/data/db/" + baseName + "2", "--sslOnNormalPorts",
                       "--sslPEMKeyFile", "jstests/libs/server.pem",
                       "--sslCAFile", "jstests/libs/ca.pem");

mongo = runMongoProgram("mongo", "--port", ports[1], "--ssl", 
                        "--eval", ";");

// 1 is the exit code for failure
assert(mongo==1);
