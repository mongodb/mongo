/**
 * This tests that updates to user and role definitions made on one mongos propagate properly
 * to other mongoses.
 */

var authzErrorCode = 13;
var st = new ShardingTest({ shards: 2,
                            config: 3,
                            mongos: [{},
                                     {setParameter: "userCacheInvalidationIntervalSecs=30"},
                                     {}],
                            keyFile: 'jstests/libs/key1' });

var res = st.s1.getDB('admin').runCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 29});
assert.commandFailed(res, "Setting the invalidation interval to an disallowed value should fail");

res = st.s1.getDB('admin').runCommand({setParameter: 1, userCacheInvalidationIntervalSecs: 100000});
assert.commandFailed(res, "Setting the invalidation interval to an disallowed value should fail");

res = st.s1.getDB('admin').runCommand({getParameter: 1, userCacheInvalidationIntervalSecs: 1});

assert.eq(30, res.userCacheInvalidationIntervalSecs);
st.s0.getDB('test').foo.insert({a:1}); // initial data

st.s0.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});
st.s0.getDB('admin').auth('admin', 'pwd');
st.s0.getDB('admin').createRole({role: 'myRole',
                                 roles: [],
                                 privileges: [{resource: {cluster: true},
                                               actions: ['invalidateUserCache']}]});


var db1 = st.s0.getDB('test');
db1.createUser({user: 'spencer', pwd: 'pwd', roles: ['read', {role: 'myRole', db: 'admin'}]});
db1.auth('spencer', 'pwd');

var db2 = st.s1.getDB('test');
db2.auth('spencer', 'pwd');

var db3 = st.s2.getDB('test');
db3.auth('spencer', 'pwd');

/**
 * At this point we have 3 handles to the "test" database, each of which are on connections to
 * different mongoses.  "db1", "db2", and "db3" are all auth'd as spencer@test and will be used
 * to verify that user and role data changes get propaged to their mongoses. "db1" is *additionally*
 * auth'd as a user with the "userAdminAnyDatabase" role.  This is the mongos that will be used to
 * modify user and role data.
 * "db2" is connected to a mongos with a 30 second user cache invalidation interval,
 * while "db3" is connected to a mongos with the default 10 minute cache invalidation interval.
 */

(function testGrantingPrivileges() {
     jsTestLog("Testing propagation of granting privileges");

     db1.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db1, authzErrorCode);
     db2.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db2, authzErrorCode);
     db3.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db3, authzErrorCode);

     assert.eq(1, db1.foo.findOne().a);
     assert.eq(1, db2.foo.findOne().a);
     assert.eq(1, db3.foo.findOne().a);


     db1.getSiblingDB('admin').grantPrivilegesToRole("myRole",
                                                     [{resource: {db: 'test', collection: ''},
                                                       actions: ['update']}]);

     // s0/db1 should update its cache instantly
     db1.foo.update({}, {$inc: {a:1}});
     assert.gleSuccess(db1);
     assert.eq(2, db1.foo.findOne().a);

     // s1/db2 should update its cache in 30 seconds.
     assert.soon(function() {
                     db2.foo.update({}, {$inc: {a:1}});
                     if (db2.getLastError()) {
                         return false;
                     }
                     return db2.foo.findOne().a == 3;
                 },
                 "Mongos did not update its user cache after 30 seconds",
                 31 * 1000); // Give an extra 1 second to avoid races

     // We manually invalidate the cache on s2/db3.
     db3.adminCommand("invalidateUserCache");
     db3.foo.update({}, {$inc: {a:1}});
     assert.gleSuccess(db3);
     assert.eq(4, db3.foo.findOne().a);

 })();

(function testRevokingPrivileges() {
     jsTestLog("Testing propagation of revoking privileges");

     db1.getSiblingDB('admin').revokePrivilegesFromRole("myRole",
                                                        [{resource: {db: 'test', collection: ''},
                                                          actions: ['update']}]);

     // s0/db1 should update its cache instantly
     db1.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db1, authzErrorCode);

     // s1/db2 should update its cache in 30 seconds.
     assert.soon(function() {
                     db2.foo.update({}, {$inc: {a:1}});
                     return db2.getLastErrorObj().code == authzErrorCode;
                 },
                 "Mongos did not update its user cache after 30 seconds",
                 31 * 1000); // Give an extra 1 second to avoid races

     // We manually invalidate the cache on s1/db3.
     db3.adminCommand("invalidateUserCache");
     db3.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db3, authzErrorCode);

 })();

(function testModifyingUser() {
     jsTestLog("Testing propagation modifications to a user, rather than to a role");

     db1.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db1, authzErrorCode);
     db2.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db2, authzErrorCode);
     db3.foo.update({}, {$inc: {a:1}});
     assert.gleErrorCode(db3, authzErrorCode);

     db1.getSiblingDB('test').grantRolesToUser("spencer", ['readWrite']);

     // s0/db1 should update its cache instantly
     db1.foo.update({}, {$inc: {a:1}});
     assert.gleSuccess(db1);

     // s1/db2 should update its cache in 30 seconds.
     assert.soon(function() {
                     db2.foo.update({}, {$inc: {a:1}});
                     return !db2.getLastError();
                 },
                 "Mongos did not update its user cache after 30 seconds",
                 31 * 1000); // Give an extra 1 second to avoid races

     // We manually invalidate the cache on s1/db3.
     db3.adminCommand("invalidateUserCache");
     db3.foo.update({}, {$inc: {a:1}});
     assert.gleSuccess(db3);

 })();

(function testDroppingUser() {
     jsTestLog("Testing propagation of dropping users");

     assert.commandWorked(db1.foo.runCommand("collStats"));
     assert.commandWorked(db2.foo.runCommand("collStats"));
     assert.commandWorked(db3.foo.runCommand("collStats"));

     db1.dropUser('spencer');

     // s0/db1 should update its cache instantly
     assert.commandFailedWithCode(db1.foo.runCommand("collStats"), authzErrorCode);

     // s1/db2 should update its cache in 30 seconds.
     assert.soon(function() {
                     return db2.foo.runCommand("collStats").code == authzErrorCode;
                 },
                 "Mongos did not update its user cache after 30 seconds",
                 31 * 1000); // Give an extra 1 second to avoid races

     // We manually invalidate the cache on s2/db3.
     db3.adminCommand("invalidateUserCache");
     assert.commandFailedWithCode(db3.foo.runCommand("collStats"), authzErrorCode);

 })();

st.stop();
