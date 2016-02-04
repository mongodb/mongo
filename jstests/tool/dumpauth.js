// dumpauth.js
// test mongodump with authentication

var m = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
var dbName = "admin";
var colName = "testcol";
var profileName = "system.profile";
var dumpDir = MongoRunner.dataPath + "jstests_tool_dumprestore_dump_system_profile/";
db = m.getDB(dbName);

db.createUser({user:  "testuser" , pwd: "testuser", roles: jsTest.adminUserRoles});
assert( db.auth( "testuser" , "testuser" ) , "auth failed" );

t = db[colName];
t.drop();
profile = db[profileName];
profile.drop();

// Activate profiling, to ensure that system.profile can be dumped with the backup role
db.setProfilingLevel(2);

// Populate the database
for(var i = 0; i < 100; i++) {
  t.save({ "x": i });
}
assert.gt(profile.count(), 0, "admin.system.profile should have documents");
assert.eq(t.count(), 100, "testcol should have documents");

// Create a user with backup permissions
db.createUser({user:  "backup" , pwd: "password", roles: ["backup"]});

// Backup the database with the backup user
x = runMongoProgram( "mongodump",
                     "--db", dbName,
                     "--out", dumpDir,
                     "--authenticationDatabase=admin",
                     "-u", "backup",
                     "-p", "password",
                     "-h", "127.0.0.1:"+m.port);
assert.eq(x, 0, "mongodump should succeed with authentication");

// Assert that a BSON document for admin.system.profile has been produced
x = runMongoProgram( "bsondump", dumpDir + "/" + dbName + "/" + profileName + ".bson" );
assert.eq(x, 0, "bsondump should succeed parsing the profile data");
