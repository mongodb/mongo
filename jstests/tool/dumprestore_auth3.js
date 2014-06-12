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
    var admindb = mongod.getDB("admin");
    var db = mongod.getDB("foo");
    var shutMessage = "(no shutdown)";
    if (shutdownServer) {
        shutMessage = "(shutdown)"
    }

    jsTestLog("Creating Admin user & initial data "+shutMessage);
    admindb.createUser({user: 'root', pwd: 'pass', roles: ['root']});
    admindb.createUser({user: 'backup', pwd: 'pass', roles: ['backup']});
    admindb.createUser({user: 'restore', pwd: 'pass', roles: ['restore']});
    db.createUser({user: 'user', pwd: 'pass', roles: jsTest.basicUserRoles});
    db.createRole({role: 'role', roles: [], privileges:[]});
    var backupActions = ['find'];
    db.createRole({role: 'backupFooChester',
       privileges: [{resource: {db: 'foo', collection: 'chester'}, actions: backupActions}],
       roles: []});
    db.createUser({user: 'backupFooChester', pwd: 'pass', roles: ['backupFooChester']});

    var userCount = db.getUsers().length;
    var rolesCount = db.getRoles().length;
    var systemUsersCount = admindb.system.users.count();
    var systemVersionCount = admindb.system.version.count();

    db.bar.insert({a:1});

    assert.eq(1, db.bar.findOne().a);
    assert.eq(userCount, db.getUsers().length, "setup");
    assert.eq(rolesCount, db.getRoles().length, "setup2");
    assert.eq(systemUsersCount, admindb.system.users.count(), "setup3");
    assert.eq(systemVersionCount, admindb.system.version.count(),"system version");
    assert.eq(1, admindb.system.users.count({user: "restore"}), "Restore user is missing");
    assert.eq(1, admindb.system.users.count({user: "backup"}), "Backup user is missing");
    var versionDoc = admindb.system.version.findOne();

    jsTestLog("Dump foo database without dumping user data "+shutMessage);
    var dumpDir = MongoRunner.getAndPrepareDumpDirectory("dumprestore_auth3");
    mongod = runTool("mongodump", mongod, shutdownServer, {out: dumpDir, db: "foo"});
    db = mongod.getDB('foo');

    db.dropDatabase();
    db.dropAllUsers();
    db.dropAllRoles();

    jsTestLog("Restore foo database from dump that doesn't contain user data "+shutMessage);
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/",
                                                              db: 'foo',
                                                              restoreDbUsersAndRoles: ""});

    db = mongod.getDB('foo');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(0, db.getUsers().length, "Restore created users somehow");
    assert.eq(0, db.getRoles().length, "Restore created roles somehow");

    // Re-create user data
    db.createUser({user: 'user', pwd: 'password', roles: jsTest.basicUserRoles});
    db.createRole({role: 'role', roles: [], privileges:[]});

    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't create user");
    assert.eq(1, db.getRoles().length, "didn't create role");

    jsTestLog("Dump foo database *with* user data "+shutMessage);
    mongod = runTool("mongodump", mongod, shutdownServer, {out: dumpDir,
                                                           db: "foo",
                                                           dumpDbUsersAndRoles: ""});
    db = mongod.getDB('foo');

    db.dropDatabase();
    db.dropAllUsers();
    db.dropAllRoles();

    assert.eq(0, db.getUsers().length, "didn't drop users");
    assert.eq(0, db.getRoles().length, "didn't drop roles");
    assert.eq(0, db.bar.count(), "didn't drop 'bar' collection");

    jsTestLog("Restore foo database without restoring user data, even though it's in the dump "+shutMessage);
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/", db: 'foo'});
    db = mongod.getDB('foo');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(0, db.getUsers().length, "Restored users even though it shouldn't have");
    assert.eq(0, db.getRoles().length, "Restored roles even though it shouldn't have");

    jsTestLog("Restore foo database *with* user data "+shutMessage);
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/",
                                                              db: 'foo',
                                                              restoreDbUsersAndRoles: ""});
    db = mongod.getDB('foo');
    admindb = mongod.getDB('admin');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't restore users");
    assert.eq(1, db.getRoles().length, "didn't restore roles");
    assert.eq(1, admindb.system.users.count({user: "restore", db: "admin"}), "Restore user is missing");
    assert.docEq(versionDoc,
                 db.getSiblingDB('admin').system.version.findOne(),
                 "version doc was changed by restore");

    jsTestLog("Make modifications to user data that should be overridden by the restore "+shutMessage);
    db.dropUser('user')
    db.createUser({user: 'user2', pwd: 'password2', roles: jsTest.basicUserRoles});
    db.dropRole('role')
    db.createRole({role: 'role2', roles: [], privileges:[]});

    jsTestLog("Restore foo database (and user data) with --drop so it overrides the changes made "+shutMessage);
    // Restore with --drop to override the changes to user data
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir + "foo/",
                                                              db: 'foo',
                                                              drop: "",
                                                              restoreDbUsersAndRoles: ""});
    db = mongod.getDB('foo');
    admindb = mongod.getDB('admin');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    // Admin users not restored: SERVER-14212
    //assert.eq(1, admindb.system.users.count({user: "restore"}), "Restore user is missing");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't restore users");
    assert.eq("user", db.getUsers()[0].user, "didn't update user");
    assert.eq(1, db.getRoles().length, "didn't restore roles");
    assert.eq("role", db.getRoles()[0].role, "didn't update role");
    assert.docEq(versionDoc,
                 db.getSiblingDB('admin').system.version.findOne(),
                 "version doc was changed by restore");


    jsTestLog("Dump just the admin database.  User data should be dumped by default "+shutMessage);
    // Make a user in another database to make sure it is properly captured
    db.getSiblingDB('bar').createUser({user: "user", pwd: 'pwd', roles: []});
    db.getSiblingDB('admin').createUser({user: "user", pwd: 'pwd', roles: []});
    mongod = runTool("mongodump", mongod, shutdownServer, {out: dumpDir, db: "admin"});
    db = mongod.getDB('foo');

    // Change user data a bit.
    db.dropAllUsers();
    db.getSiblingDB('bar').createUser({user: "user2", pwd: 'pwd', roles: []});
    db.getSiblingDB('admin').dropAllUsers();

    jsTestLog("Restore just the admin database. User data should be restored by default "+shutMessage);
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
    assert.eq(1, db.getUsers().length, "didn't restore users for bar database");
    assert.eq("user", db.getUsers()[0].user, "didn't restore user for bar database");
    assert.eq(1, admindb.getUsers().length, "didn't restore users for admin database");
    assert.eq("user", admindb.getUsers()[0].user, "didn't restore user for admin database");
    assert.eq(3, admindb.system.users.count(), "has the wrong # of users for the whole server");
    assert.eq(1, admindb.system.roles.count(), "has the wrong # of roles for the whole server");
    assert.docEq(versionDoc,
                 db.getSiblingDB('admin').system.version.findOne(),
                 "version doc was changed by restore");


    jsTestLog("Dump all databases "+shutMessage);
    mongod = runTool("mongodump", mongod, shutdownServer, {out: dumpDir});
    db = mongod.getDB('foo');

    db.dropDatabase();
    db.dropAllUsers();
    db.dropAllRoles();

    assert.eq(0, db.getUsers().length, "didn't drop users");
    assert.eq(0, db.getRoles().length, "didn't drop roles");
    assert.eq(0, db.bar.count(), "didn't drop 'bar' collection");

    jsTestLog("Restore all databases "+shutMessage);
    mongod = runTool("mongorestore", mongod, shutdownServer, {dir: dumpDir});
    db = mongod.getDB('foo');

    assert.soon(function() { return db.bar.findOne(); }, "no data after restore");
    assert.eq(1, db.bar.findOne().a);
    assert.eq(1, db.getUsers().length, "didn't restore users");
    assert.eq(1, db.getRoles().length, "didn't restore roles");
    assert.docEq(versionDoc,
                 db.getSiblingDB('admin').system.version.findOne(),
                 "version doc was changed by restore");

    MongoRunner.stopMongod(mongod);
}

runTest(false);
runTest(true);
