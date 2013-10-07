// Test passwords on private keys for SSL
// This tests that providing a proper password works and that providing no password or incorrect
// password fails.  It uses both mongod and mongo to run the tests, since the mongod binary
// does not return error statuses to indicate an error.
port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_ssl_ssl_cert_password";
var dbpath = "/data/db/" + baseName;
var external_scratch_dir = "/data/db/" + baseName + "/external/";
resetDbpath(dbpath);
mkdir(external_scratch_dir);

// Password is correct
md = startMongod("--nopreallocj",
                 "--port", port, 
                 "--dbpath", dbpath, 
                 "--sslMode","sslOnly",
                 "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                 "--sslPEMKeyPassword", "qwerty");
// startMongod connects a Mongo shell, so if we get here, the test is successful.


// Password incorrect; error logged is:
//  error:06065064:digital envelope routines:EVP_DecryptFinal_ex:bad decrypt
var exit_code = runMongoProgram("mongo", "--port", port,
                                "--ssl",
                                "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                                "--sslPEMKeyPassword", "barf");

// 1 is the exit code for failure
assert(exit_code == 1);

// Test that mongodump and mongorestore support ssl
c = md.getDB("dumprestore_ssl").getCollection("foo");
assert.eq(0, c.count(), "dumprestore_ssl.foo collection is not initially empty");
c.save({ a : 22 });
assert.eq(1, c.count(), "failed to insert document into dumprestore_ssl.foo collection");

exit_code = runMongoProgram("mongodump", "--out", external_scratch_dir,
                            "--port", port,
                            "--ssl",
                            "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                            "--sslPEMKeyPassword", "qwerty");

assert.eq(exit_code, 0, "Failed to start mongodump with ssl");

c.drop();
assert.eq(0, c.count(), "dumprestore_ssl.foo collection is not empty after drop");

exit_code = runMongoProgram("mongorestore", "--dir", external_scratch_dir,
                            "--port", port,
                            "--ssl",
                            "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                            "--sslPEMKeyPassword", "qwerty");

assert.eq(exit_code, 0, "Failed to start mongorestore with ssl");

assert.soon("c.findOne()", "no data after sleep.  Expected a document after calling mongorestore");
assert.eq(1, c.count(),
          "did not find expected document in dumprestore_ssl.foo collection after mongorestore");
assert.eq(22, c.findOne().a,
          "did not find correct value in document after mongorestore");

// Test that mongoimport and mongoexport support ssl
var exportimport_ssl_dbname = "exportimport_ssl";
c = md.getDB(exportimport_ssl_dbname).getCollection("foo");
assert.eq(0, c.count(), "exportimport_ssl.foo collection is not initially empty");
c.save({ a : 22 });
assert.eq(1, c.count(), "failed to insert document into exportimport_ssl.foo collection");

var exportimport_file = "data.json";

exit_code = runMongoProgram("mongoexport", "--out", external_scratch_dir + exportimport_file,
                            "-d", exportimport_ssl_dbname, "-c", "foo",
                            "--port", port,
                            "--ssl",
                            "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                            "--sslPEMKeyPassword", "qwerty");

assert.eq(exit_code, 0, "Failed to start mongoexport with ssl");

c.drop();
assert.eq(0, c.count(), "afterdrop", "-d", exportimport_ssl_dbname, "-c", "foo");

exit_code = runMongoProgram("mongoimport", "--file", external_scratch_dir + exportimport_file,
                            "-d", exportimport_ssl_dbname, "-c", "foo",
                            "--port", port,
                            "--ssl",
                            "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                            "--sslPEMKeyPassword", "qwerty");

assert.eq(exit_code, 0, "Failed to start mongoimport with ssl");

assert.soon("c.findOne()", "no data after sleep.  Expected a document after calling mongoimport");
assert.eq(1, c.count(),
          "did not find expected document in dumprestore_ssl.foo collection after mongoimport");
assert.eq(22, c.findOne().a,
          "did not find correct value in document after mongoimport");

// Test that mongofiles supports ssl
var mongofiles_ssl_dbname = "mongofiles_ssl"
mongofiles_db = md.getDB(mongofiles_ssl_dbname);

source_filename = 'jstests/ssl/ssl_cert_password.js'
filename = 'ssl_cert_password.js'

exit_code = runMongoProgram("mongofiles", "-d", mongofiles_ssl_dbname, "put", source_filename,
                            "--port", port,
                            "--ssl",
                            "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                            "--sslPEMKeyPassword", "qwerty");

assert.eq(exit_code, 0, "Failed to start mongofiles with ssl");

md5 = md5sumFile(source_filename);

file_obj = mongofiles_db.fs.files.findOne()
assert(file_obj, "failed to find file object in mongofiles_ssl db using gridfs");
md5_stored = file_obj.md5;
md5_computed = mongofiles_db.runCommand({filemd5: file_obj._id}).md5;
assert.eq(md5, md5_stored, "md5 incorrect for file");
assert.eq(md5, md5_computed, "md5 computed incorrectly by server");

exit_code = runMongoProgram("mongofiles", "-d", mongofiles_ssl_dbname, "get", source_filename,
                            "-l", external_scratch_dir + filename,
                            "--port", port,
                            "--ssl",
                            "--sslPEMKeyFile", "jstests/libs/password_protected.pem",
                            "--sslPEMKeyPassword", "qwerty");

assert.eq(exit_code, 0, "Failed to start mongofiles with ssl");

md5 = md5sumFile(external_scratch_dir + filename);
assert.eq(md5, md5_stored, "hash of stored file does not match the expected value");

if (!_isWindows()) {
    // Stop the server
    var exitCode = stopMongod(port, 15);
    assert(exitCode == 0);
}
