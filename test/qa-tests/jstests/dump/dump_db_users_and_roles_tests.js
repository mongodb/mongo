if (typeof getToolTest === 'undefined') {
  load('jstests/configs/plain_28.config.js');
}

(function() {
  resetDbpath('dump');
  var toolTest = getToolTest('dumpDbUsersAndRolesTest');
  var commonToolArgs = getCommonToolArguments();
  var db = toolTest.db.getSiblingDB('foo');

  db.dropDatabase();
  assert.eq(0, db.bar.count());

  db.getSiblingDB('baz').dropDatabase();
  assert.eq(0, db.getSiblingDB('baz').bar.count());

  // Create roles
  db.createRole({
    role: 'taco',
    privileges: [
      { resource: { db: 'foo', collection: '' }, actions: ['find'] }
    ],
    roles: []
  });
  db.getSiblingDB('baz').createRole({
    role: 'bacon',
    privileges: [
      { resource: { db: 'baz', collection: '' }, actions: ['find'] }
    ],
    roles: []
  });

  // And users with those roles
  db.createUser({
    user: 'passwordIsTaco',
    pwd: 'Taco',
    roles: [{ role: 'taco', db: 'foo' }]
  });
  db.getSiblingDB('baz').createUser({
    user: 'eggs',
    pwd: 'bacon',
    roles: [{ role: 'bacon', db: 'baz' }]
  });

  // Running mongodump with `--db foo --dumpDbUsersAndRoles` should dump the
  // associated users
  resetDbpath('dump');
  var dumpArgs = ['dump', '--db', 'foo', '--dumpDbUsersAndRoles'].
    concat(commonToolArgs);
  toolTest.runTool.apply(toolTest, dumpArgs);
  db.dropDatabase();
  db.getSiblingDB('baz').dropDatabase();
  db.getSiblingDB('admin').dropDatabase();

  var restoreArgs = ['restore', '--restoreDbUsersAndRoles'].
    concat(commonToolArgs);
  toolTest.runTool.apply(toolTest, restoreArgs);
  var c = db.getSiblingDB('admin').system.roles.find();

  // Should have restored only the user that was in the 'foo' db
  assert.eq(1, db.getSiblingDB('admin').system.users.count());
  assert.eq('passwordIsTaco',
    db.getSiblingDB('admin').system.users.findOne().user);
  // And only the role that was defined on the 'foo' db
  assert.eq(1, db.getSiblingDB('admin').system.roles.count());
  assert.eq('taco', db.getSiblingDB('admin').system.roles.findOne().role);

  toolTest.stop();
})();
