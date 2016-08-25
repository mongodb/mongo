// This test requires mongo 2.6.x releases
// @tags: [requires_mongo_26]
(function() {

  load("jstests/configs/standard_dump_targets.config.js");

  // Tests using mongorestore to restore a dump containing users. If there is
  // conflicting authSchemaVersion in the admin.system.version document, it
  // should be ignored, and the restore should complete successfully.

  jsTest.log('Testing restoring a dump with a potentially conflicting'+
            ' authSchemaVersion in the database');

  if (dump_targets !== "standard") {
    print('skipping test incompatable with archiving or compression');
    return assert(true);
  }

  var runTest = function(sourceDBVersion, dumpVersion, restoreVersion, destDBVersion, shouldSucceed) {

    jsTest.log('Running with sourceDBVersion=' + (sourceDBVersion || 'latest') +
                ', dumpVersion=' + (dumpVersion || 'latest') + ', restoreVersion=' +
                (restoreVersion || 'latest') + ', and destDBVersion=' +
                (destDBVersion || 'latest') + ', expected to pass=' + shouldSucceed);

    var toolTest = new ToolTest('conflicting_auth_schema_version',
            {binVersion: sourceDBVersion, auth: ''});
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = 'conflicting_auth_schema_version_dump';
    resetDbpath(dumpTarget);

    // the admin db, and the non-admin db we'll be using
    var adminDB = toolTest.db.getSiblingDB('admin');
    var testDB = toolTest.db.getSiblingDB('test');

    // create a user admin
    adminDB.createUser({
      user: 'admin',
      pwd: 'password',
      roles: [
        {role: 'userAdminAnyDatabase', db: 'admin'},
        {role: 'readWriteAnyDatabase', db: 'admin'},
        {role: 'backup', db: 'admin'},
      ],
    });
    var authInfo = {user: 'admin', pwd: 'password'};
    if (sourceDBVersion === "2.6") {
      authInfo.mechanism = "MONGODB-CR";
    }
    assert.eq(1, adminDB.auth(authInfo));

    // add some data
    for (var i = 0; i < 10; i++) {
      testDB.data.insert({_id: i});
    }

    // sanity check the data was inserted
    assert.eq(10, testDB.data.count());

    // dump all the data
    args = ['mongodump' + (dumpVersion ? ('-'+dumpVersion) : ''),
      '--username', 'admin',
      '--password', 'password', '--port', toolTest.port]
      .concat(getDumpTarget(dumpTarget));
    if (sourceDBVersion === "2.6") {
      args.push("--authenticationMechanism=MONGODB-CR");
    }
    var ret = runMongoProgram.apply(this, args);
    assert.eq(0, ret);

    // restart the mongod, with a clean db path
    stopMongod(toolTest.port);
    resetDbpath(toolTest.dbpath);
    toolTest.m = null;
    toolTest.db = null;
    toolTest.options.binVersion = destDBVersion;
    toolTest.startDB('foo');

    // refresh the db references
    adminDB = toolTest.db.getSiblingDB('admin');
    testDB = toolTest.db.getSiblingDB('test');

    // create a new user admin
    adminDB.createUser({
      user: 'admin28',
      pwd: 'password',
      roles: [
        {role: 'userAdminAnyDatabase', db: 'admin'},
        {role: 'readWriteAnyDatabase', db: 'admin'},
        {role: 'restore', db: 'admin'},
      ],
    });

    var authInfoDest = {user: 'admin28', pwd: 'password'};
    if (destDBVersion === "2.6") {
      authInfoDest.mechanism = "MONGODB-CR";
    }
    assert.eq(1, adminDB.auth(authInfoDest));

    // do a full restore
    args = ['mongorestore' + (restoreVersion ? ('-'+restoreVersion) : ''),
      '--username', 'admin28',
      '--password', 'password',
      '--port', toolTest.port,
      '--stopOnError']
      .concat(getRestoreTarget(dumpTarget));

    ret = runMongoProgram.apply(this, args);

    if (shouldSucceed) {
      assert.eq(0, ret);
      // make sure the data and users are all there
      assert.eq(10, testDB.data.count());
      for (i = 0; i < 10; i++) {
        assert.eq(1, testDB.data.count({_id: i}));
      }
      var users = adminDB.getUsers();
      assert.eq(2, users.length);
      assert(users[0].user === 'admin' || users[1].user === 'admin');
      assert(users[0].user === 'admin28' || users[1].user === 'admin28');
    } else {
      assert.neq(0, ret);
    }
    // success
    toolTest.stop();
  };

  // 'undefined' triggers latest
  runTest('2.6', '2.6', undefined, '2.6', true);
  runTest('2.6', '2.6', undefined, undefined, true);
  runTest('2.6', undefined, undefined, undefined, true);
  runTest(undefined, undefined, undefined, '2.6', false);
  runTest(undefined, undefined, undefined, undefined, true);
}());
