(function() {

  load("jstests/configs/standard_dump_targets.config.js");

  // Tests running mongorestore with --drop and --restoreDbUsersAndRoles,
  // in addition to --auth, and makes sure the authenticated user does not
  // get dropped before it can complete the restore job.

  jsTest.log('Testing dropping the authenticated user with mongorestore');

  var toolTest = new ToolTest('drop_authenticated_user', {auth: ''});
  toolTest.startDB('foo');

  // where we'll put the dump
  var dumpTarget = 'drop_authenticated_user_dump';
  resetDbpath(dumpTarget);

  // we'll use the admin db so that the user we are restoring as
  // is part of the db we are restoring
  var adminDB = toolTest.db.getSiblingDB('admin');

  // create the users we'll need for the dump
  adminDB.createUser({
    user: 'admin',
    pwd: 'password',
    roles: [
      {role: 'userAdmin', db: 'admin'},
      {role: 'readWrite', db: 'admin'},
    ],
  });
  adminDB.auth('admin', 'password');

  adminDB.createUser({
    user: 'backup',
    pwd: 'password',
    roles: [{role: 'backup', db: 'admin'}],
  });

  // create a role
  adminDB.createRole({
    role: 'extraRole',
    privileges: [{
      resource: {db: 'admin', collection: ''},
      actions: ['find'],
    }],
    roles: [],
  });

  // insert some data
  for (var i = 0; i < 10; i++) {
    adminDB.data.insert({_id: i});
  }
  // sanity check the insertion worked
  assert.eq(10, adminDB.data.count());

  // dump the data
  var ret = toolTest.runTool.apply(toolTest, ['dump',
      '--username', 'backup',
      '--password', 'password']
    .concat(getDumpTarget(dumpTarget)));
  assert.eq(0, ret);

  // drop all the data, but not the users or roles
  adminDB.data.remove({});
  // sanity check the removal worked
  assert.eq(0, adminDB.data.count());

  // now create the restore user, so that we can use it for the restore but it is
  // not part of the dump
  adminDB.createUser({
    user: 'restore',
    pwd: 'password',
    roles: [{role: 'restore', db: 'admin'}],
  });

  // insert some data to be removed when --drop is run
  for (i = 10; i < 20; i++) {
    adminDB.data.insert({_id: i});
  }
  // sanity check the insertion worked
  assert.eq(10, adminDB.data.count());

  // restore the data, specifying --drop
  ret = toolTest.runTool.apply(toolTest, ['restore',
      '--drop',
      '--username', 'restore',
      '--password', 'password']
    .concat(getRestoreTarget(dumpTarget)));
  assert.eq(0, ret);

  // make sure the existing data was removed, and replaced with the dumped data
  assert.eq(10, adminDB.data.count());
  for (i = 0; i < 10; i++) {
    assert.eq(1, adminDB.data.count({_id: i}));
  }

  // make sure the correct roles and users exist - that the restore user was dropped
  var users = adminDB.getUsers();
  assert.eq(2, users.length);
  assert(users[0].user === 'backup' || users[1].user === 'backup');
  assert(users[0].user === 'admin' || users[1].user === 'admin');
  assert.eq(1, adminDB.getRoles().length);

  // success
  toolTest.stop();

}());
