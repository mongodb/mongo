// Test passwords on private keys for SSL
// This tests that providing a proper password works and that providing no password or incorrect
// password fails.  It uses both bongod and bongo to run the tests, since the bongod binary
// does not return error statuses to indicate an error.
// This test requires ssl support in bongo-tools
// @tags: [requires_ssl_bongo_tools]
var baseName = "jstests_ssl_ssl_cert_password";
var dbpath = BongoRunner.dataPath + baseName;
var external_scratch_dir = BongoRunner.dataPath + baseName + "/external/";
resetDbpath(dbpath);
mkdir(external_scratch_dir);

// Password is correct
var md = BongoRunner.runBongod({
    nopreallocj: "",
    dbpath: dbpath,
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/password_protected.pem",
    sslPEMKeyPassword: "qwerty"
});
// BongoRunner.runBongod connects a Bongo shell, so if we get here, the test is successful.

// Password incorrect; error logged is:
//  error:06065064:digital envelope routines:EVP_DecryptFinal_ex:bad decrypt
var exit_code = runBongoProgram("bongo",
                                "--port",
                                md.port,
                                "--ssl",
                                "--sslAllowInvalidCertificates",
                                "--sslCAFile",
                                "jstests/libs/ca.pem",
                                "--sslPEMKeyFile",
                                "jstests/libs/password_protected.pem",
                                "--sslPEMKeyPassword",
                                "barf");

// 1 is the exit code for failure
assert(exit_code == 1);

// Test that bongodump and bongorestore support ssl
c = md.getDB("dumprestore_ssl").getCollection("foo");
assert.eq(0, c.count(), "dumprestore_ssl.foo collection is not initially empty");
c.save({a: 22});
assert.eq(1, c.count(), "failed to insert document into dumprestore_ssl.foo collection");

exit_code = BongoRunner.runBongoTool("bongodump", {
    out: external_scratch_dir,
    port: md.port,
    ssl: "",
    sslPEMKeyFile: "jstests/libs/password_protected.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslPEMKeyPassword: "qwerty",
});

assert.eq(exit_code, 0, "Failed to start bongodump with ssl");

c.drop();
assert.eq(0, c.count(), "dumprestore_ssl.foo collection is not empty after drop");

exit_code = BongoRunner.runBongoTool("bongorestore", {
    dir: external_scratch_dir,
    port: md.port,
    ssl: "",
    sslCAFile: "jstests/libs/ca.pem",
    sslPEMKeyFile: "jstests/libs/password_protected.pem",
    sslPEMKeyPassword: "qwerty",
});

assert.eq(exit_code, 0, "Failed to start bongorestore with ssl");

assert.soon("c.findOne()", "no data after sleep.  Expected a document after calling bongorestore");
assert.eq(1,
          c.count(),
          "did not find expected document in dumprestore_ssl.foo collection after bongorestore");
assert.eq(22, c.findOne().a, "did not find correct value in document after bongorestore");

// Test that bongoimport and bongoexport support ssl
var exportimport_ssl_dbname = "exportimport_ssl";
c = md.getDB(exportimport_ssl_dbname).getCollection("foo");
assert.eq(0, c.count(), "exportimport_ssl.foo collection is not initially empty");
c.save({a: 22});
assert.eq(1, c.count(), "failed to insert document into exportimport_ssl.foo collection");

var exportimport_file = "data.json";

exit_code = BongoRunner.runBongoTool("bongoexport", {
    out: external_scratch_dir + exportimport_file,
    db: exportimport_ssl_dbname,
    collection: "foo",
    port: md.port,
    ssl: "",
    sslCAFile: "jstests/libs/ca.pem",
    sslPEMKeyFile: "jstests/libs/password_protected.pem",
    sslPEMKeyPassword: "qwerty",
});

assert.eq(exit_code, 0, "Failed to start bongoexport with ssl");

c.drop();
assert.eq(0, c.count(), "afterdrop", "-d", exportimport_ssl_dbname, "-c", "foo");

exit_code = BongoRunner.runBongoTool("bongoimport", {
    file: external_scratch_dir + exportimport_file,
    db: exportimport_ssl_dbname,
    collection: "foo",
    port: md.port,
    ssl: "",
    sslCAFile: "jstests/libs/ca.pem",
    sslPEMKeyFile: "jstests/libs/password_protected.pem",
    sslPEMKeyPassword: "qwerty",
});

assert.eq(exit_code, 0, "Failed to start bongoimport with ssl");

assert.soon("c.findOne()", "no data after sleep.  Expected a document after calling bongoimport");
assert.eq(1,
          c.count(),
          "did not find expected document in dumprestore_ssl.foo collection after bongoimport");
assert.eq(22, c.findOne().a, "did not find correct value in document after bongoimport");

// Test that bongofiles supports ssl
var bongofiles_ssl_dbname = "bongofiles_ssl";
bongofiles_db = md.getDB(bongofiles_ssl_dbname);

source_filename = 'jstests/ssl/ssl_cert_password.js';
filename = 'ssl_cert_password.js';

exit_code = BongoRunner.runBongoTool("bongofiles",
                                     {
                                       db: bongofiles_ssl_dbname,
                                       port: md.port,
                                       ssl: "",
                                       sslCAFile: "jstests/libs/ca.pem",
                                       sslPEMKeyFile: "jstests/libs/password_protected.pem",
                                       sslPEMKeyPassword: "qwerty",
                                     },
                                     "put",
                                     source_filename);

assert.eq(exit_code, 0, "Failed to start bongofiles with ssl");

md5 = md5sumFile(source_filename);

file_obj = bongofiles_db.fs.files.findOne();
assert(file_obj, "failed to find file object in bongofiles_ssl db using gridfs");
md5_stored = file_obj.md5;
md5_computed = bongofiles_db.runCommand({filemd5: file_obj._id}).md5;
assert.eq(md5, md5_stored, "md5 incorrect for file");
assert.eq(md5, md5_computed, "md5 computed incorrectly by server");

exit_code = BongoRunner.runBongoTool("bongofiles",
                                     {
                                       db: bongofiles_ssl_dbname,
                                       local: external_scratch_dir + filename,
                                       port: md.port,
                                       ssl: "",
                                       sslCAFile: "jstests/libs/ca.pem",
                                       sslPEMKeyFile: "jstests/libs/password_protected.pem",
                                       sslPEMKeyPassword: "qwerty",
                                     },
                                     "get",
                                     source_filename);

assert.eq(exit_code, 0, "Failed to start bongofiles with ssl");

md5 = md5sumFile(external_scratch_dir + filename);
assert.eq(md5, md5_stored, "hash of stored file does not match the expected value");

if (!_isWindows()) {
    // Stop the server
    var exitCode = BongoRunner.stopBongod(md.port, 15);
    assert(exitCode == 0);
}
