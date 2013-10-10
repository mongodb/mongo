// Test mongod start with FIPS mode enabled

ports = allocatePorts(1);
port1 = ports[0];
var baseName = "jstests_ssl_ssl_fips";


var md = startMongod("--port", port1, "--dbpath",
                     "/data/db/" + baseName, "--sslOnNormalPorts",
                     "--sslPEMKeyFile", "jstests/libs/server.pem",
                     "--sslFIPSMode");

var mongo = runMongoProgram("mongo", "--port", port1, "--ssl",
                            "--sslPEMKeyFile", "jstests/libs/client.pem",
                            "--sslFIPSMode",
                            "--eval", ";");

// 0 is the exit code for success
assert(mongo==0);

