// Test passwords on private keys for SSL
// This tests that providing a proper password works and that providing no password or incorrect
// password fails.  It uses both mongod and mongo to run the tests, since the mongod binary
// does not return error statuses to indicate an error.
port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_ssl_ssl_cert_password";
var dbpath = "/data/db/" + baseName;
resetDbpath(dbpath);

// Password is correct
md = startMongod("--nopreallocj",
                 "--port", port, 
                 "--dbpath", dbpath, 
                 "--sslOnNormalPorts",
                 "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                 "--sslPEMKeyPassword", "qwerty");
// startMongod connects a Mongo shell, so if we get here, the test is successful.


// Password incorrect; error logged is:
//  error:06065064:digital envelope routines:EVP_DecryptFinal_ex:bad decrypt
md = runMongoProgram("mongo", "--port", port, 
                     "--ssl",
                     "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                     "--sslPEMKeyPassword", "barf");

// 1 is the exit code for failure
assert(md==1);


if (!_isWindows()) {
    // Stop the server
    var exitCode = stopMongod(port, 15);
    assert(exitCode == 0);
}
