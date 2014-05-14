// dumprestore_auth3.js
// Tests that mongodump and mongorestore properly handle access control information when doing
// single-db dumps and restores

// Runs the tool with the given name against the given mongod.  If shutdownServer is true,
// first shuts down the mongod and uses the --dbpath option to the tool to operate on the data
// files directly
function runTool(toolName, mongod, shutdownServer, options) {
    if (shutdownServer) {
        MongoRunner.stopMongod(mongod);
        var opts = {dbpath: mongod.fullOptions.pathOpts.dbpath};
        Object.extend(opts, options);
        assert(!MongoRunner.runMongoTool(toolName, opts));
        mongod.fullOptions.restart = true;
        return MongoRunner.runMongod(mongod.fullOptions);
    } else {
        var opts = {host: mongod.host};
        Object.extend(opts, options);
        assert(!MongoRunner.runMongoTool(toolName, opts));
        return mongod;
    }
}

// If shutdownServer is true, will run tools against shut down mongod, operating on the data
// files directly
function runTest(shutdownServer) {
    var mongod = MongoRunner.runMongod();
    var db = mongod.getDB("foo");

    jsTestLog("Creating initial data");
    db.createUser({user: 'user', pwd: 'password', roles: jsTest.basicUserRoles});
    db.createRole({role: 'role', roles: [], privileges:[]});
    // Legacy system.users collections should still be handled properly
    db.system.users.insert({user:'dbuser', pwd: 'pwd', roles: ['readWrite']});
    db.bar.insert({a:1});

    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "setup");
    assert.eq(1, db.getRoles().length, "setup2");
    assert.eq(1, db.system.users.count(), "setup3");
    assert.eq(1, db.getSiblingDB('admin').system.version.count());
    var versionDoc = db.getSiblingDB('admin').system.version.findOne();

    jsTestLog("Dump foo database without dumping user data");
    var dumpDir = MongoRunner.getAndPrepareDumpDirectory("dumprestore_auth3");
    mongod = runTool("mongodump", mongod, shutdownServer, {out: dumpDir, db: "foo"});
    db = mongod.getDB('foo');

    db.dropDatabase();
    db.dropAllUsers();
    db.dropAllRoles();

    assert.eq(0, db.getUsers().length, "didn't drop users");
    assert.eq(0, db.getRoles().length, "didn't drop roles");
    assert.eq(0, db.system.users.count(), "didn't drop legacy system.users collection");
    assert.eq(0, db.bar.count(), "didn't drop 'bar' collection");


    jsTestLog("Restore foo database from dump that doesn't contain user data");
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/",
                                                              db: 'foo',
                                                              restoreDbUsersAndRoles: ""});
    db = mongod.getDB('foo');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(0, db.getUsers().length, "Restore created users somehow");
    assert.eq(0, db.getRoles().length, "Restore created roles somehow");
    assert.eq(0, db.system.users.count(), "Restore created legacy system.users collection somehow");

    // Re-create user data
    db.createUser({user: 'user', pwd: 'password', roles: jsTest.basicUserRoles});
    db.createRole({role: 'role', roles: [], privileges:[]});
    assert.writeOK(db.system.users.insert({user:'dbuser', pwd: 'pwd', roles: ['readWrite']}));

    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't create user");
    assert.eq(1, db.getRoles().length, "didn't create role");
    assert.eq(1, db.system.users.count(), "didn't create legacy system.users collection");


    jsTestLog("Dump foo database *with* user data");
    mongod = runTool("mongodump", mongod, shutdownServer, {out: dumpDir,
                                                           db: "foo",
                                                           dumpDbUsersAndRoles: ""});
    db = mongod.getDB('foo');

    db.dropDatabase();
    db.dropAllUsers();
    db.dropAllRoles();

    assert.eq(0, db.getUsers().length, "didn't drop users");
    assert.eq(0, db.getRoles().length, "didn't drop roles");
    assert.eq(0, db.system.users.count(), "didn't drop legacy system.users collection");
    assert.eq(0, db.bar.count(), "didn't drop 'bar' collection");

    jsTestLog("Restore foo database without restoring user data, even though it's in the dump");
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/", db: 'foo'});
    db = mongod.getDB('foo');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(0, db.getUsers().length, "Restored users even though it shouldn't have");
    assert.eq(0, db.getRoles().length, "Restored users even though it shouldn't have");

    jsTestLog("Restore foo database *with* user data");
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/",
                                                              db: 'foo',
                                                              restoreDbUsersAndRoles: ""});
    db = mongod.getDB('foo');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't restore users");
    assert.eq(1, db.getRoles().length, "didn't restore roles");
    assert.eq(1, db.system.users.count(), "didn't restore legacy system.users collection");
    assert.docEq(versionDoc,
                 db.getSiblingDB('admin').system.version.findOne(),
                 "version doc was changed by restore");


    jsTestLog("Make modifications to user data that should be overridden by the restore");
    db.dropUser('user')
    db.createUser({user: 'user2', pwd: 'password2', roles: jsTest.basicUserRoles});
    db.dropRole('role')
    db.createRole({role: 'role2', roles: [], privileges:[]});
    db.system.users.remove({});
    db.system.users.insert({user:'dbuser2', pwd: 'pwd', roles: ['readWrite']});

    jsTestLog("Restore foo database (and user data) with --drop so it overrides the changes made");
    // Restore with --drop to override the changes to user data
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/",
                                                              db: 'foo',
                                                              drop: "",
                                                              restoreDbUsersAndRoles: ""});
    db = mongod.getDB('foo');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't restore users");
    assert.eq("user", db.getUsers()[0].user, "didn't update user");
    assert.eq(1, db.getRoles().length, "didn't restore roles");
    assert.eq("role", db.getRoles()[0].role, "didn't update role");
    assert.eq(1, db.system.users.count(), "didn't restore legacy system.users collection");
    assert.eq("dbuser", db.system.users.findOne().user, "didn't update legacy user");
    assert.docEq(versionDoc,
                 db.getSiblingDB('admin').system.version.findOne(),
                 "version doc was changed by restore");


    jsTestLog("Dump just the admin database.  User data should be dumped by default");
    // Make a user in another database to make sure it is properly captured
    db.getSiblingDB('bar').createUser({user: "user", pwd: 'pwd', roles: []});
    db.getSiblingDB('admin').createUser({user: "user", pwd: 'pwd', roles: []});
    mongod = runTool("mongodump", mongod, shutdownServer, {out: dumpDir, db: "admin"});
    db = mongod.getDB('foo');

    // Change user data a bit.
    db.dropAllUsers();
    db.getSiblingDB('bar').createUser({user: "user2", pwd: 'pwd', roles: []});
    db.getSiblingDB('admin').dropAllUsers();

    jsTestLog("Restore just the admin database. User data should be restored by default");
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "admin/",
                                                              db: 'admin',
                                                              drop: ""});
    db = mongod.getDB('foo');
    var otherdb = db.getSiblingDB('bar');
    var admindb = db.getSiblingDB('admin');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't restore users");
    assert.eq("user", db.getUsers()[0].user, "didn't restore user");
    assert.eq(1, db.getRoles().length, "didn't restore roles");
    assert.eq("role", db.getRoles()[0].role, "didn't restore role");
    assert.eq(1, db.system.users.count(), "didn't restore legacy system.users collection");
    assert.eq("dbuser", db.system.users.findOne().user, "didn't restore legacy user");
    assert.eq(1, db.getUsers().length, "didn't restore users for bar database");
    assert.eq("user", db.getUsers()[0].user, "didn't restore user for bar database");
    assert.eq(1, admindb.getUsers().length, "didn't restore users for admin database");
    assert.eq("user", admindb.getUsers()[0].user, "didn't restore user for admin database");
    assert.eq(3, admindb.system.users.count(), "has the wrong # of users for the whole server");
    assert.eq(1, admindb.system.roles.count(), "has the wrong # of roles for the whole server");
    assert.docEq(versionDoc,
                 db.getSiblingDB('admin').system.version.findOne(),
                 "version doc was changed by restore");


    MongoRunner.stopMongod(mongod);
}

runTest(false);
runTest(true);
