// SERVER-15515
// Mixed version replSet, 2.4 primary & 2.6 secondary
// Updates to users are replicated

var authSucceed = 1;
var authFail = 0;

function authUser(value, conn, user, pwd) {
    var message = "Auth for user "+user+" on connection "+tojson(conn);
    assert.eq(conn.auth(user, pwd), value, message);
}

//Runs in mongo-2.6 shell
var nodes={n1: {binVersion: "2.4"},
           n2: {binVersion: "2.6"},
           n3: {binVersion: "2.6"}};
var rst = new ReplSetTest({ nodes : nodes });

// Set 2.4 node as primary
var cfg = rst.getReplSetConfig();
cfg.members[0].priority = 100;
cfg.members[1].priority = 10;
cfg.members[2].priority = 10;
rst.startSet();
rst.initiate(cfg);

// Primary
var primary = rst.getPrimary();
var priAdmin = primary.getDB("admin");
var priTest = primary.getDB("test");

// Secondary
var secondary = rst.getSecondary();
var secAdmin = secondary.getDB("admin");
var secTest = secondary.getDB("test");

// Create admin users and authenticate
var wc = {w: 3};
var admin = {user: "admin", pwd: "admin", roles: ["root"], writeConcern: wc};
var admin2 = {user: "admin2", pwd: "admin2", roles: ["dbAdminAnyDatabase"], writeConcern: wc};
priAdmin.addUser(admin);
authUser(authSucceed, priAdmin, "admin", "admin");
authUser(authSucceed, secAdmin, "admin", "admin");

priAdmin.addUser(admin2);
authUser(authSucceed, priAdmin, "admin2", "admin2");
authUser(authSucceed, secAdmin, "admin2", "admin2");

// Users
var user1 = {user: "user1", pwd: "user1", roles: ["read"], writeConcern: wc};
priTest.addUser(user1);
authUser(authSucceed, priTest, "user1", "user1");
authUser(authSucceed, secTest, "user1", "user1");

// Change password
// Due to SERVER-15441 , must assign db
db = priAdmin;
priAdmin.changeUserPassword("admin2", "ADMIN2", wc);
authUser(authFail, priAdmin, "admin2", "admin2");
authUser(authFail, secAdmin, "admin2", "admin2");
authUser(authSucceed, priAdmin, "admin2", "ADMIN2");
authUser(authSucceed, secAdmin, "admin2", "ADMIN2");

db = priTest;
priTest.changeUserPassword("user1", "USER1", wc);
authUser(authFail, priTest, "user1", "user1");
authUser(authFail, secTest, "user1", "user1");
authUser(authSucceed, priTest, "user1", "USER1");
authUser(authSucceed, secTest, "user1", "USER1");

// Change roles
var newRole = "readWrite";
priTest.system.users.update({user: "user1"}, {$set: {roles: [newRole]}}, {writeConcern: wc});

// SERVER-15513
/*
assert.eq(priTest.getUser("user1").roles[0].role, newRole, "Primary - Role change");
assert.eq(secTest.getUser("user1").roles[0].role, newRole, "Secondary- Role change");
*/
var user1Info = priTest.getUsers()[0];
assert.eq(user1Info.user, "user1", "Primary - user");
assert.eq(user1Info.roles[0], newRole, "Primary - Role change");
user1Info = secTest.getUsers()[0];
assert.eq(user1Info.user, "user1", "Secondary - user");
assert.eq(user1Info.roles[0], newRole, "Secondary - Role change");

authUser(authSucceed, priTest, "user1", "USER1");
authUser(authSucceed, secTest, "user1", "USER1");


// Stop set
rst.stopSet();
