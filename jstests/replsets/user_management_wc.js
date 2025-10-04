/**
 * This file tests that user management commands accept write concern and wait for it properly.
 * It tests that an invalid write concern leads to a writeConcernError, as well as an ok status
 * of 0 to support backward compatibility. It also tests that a valid write concern works and does
 * not yield any writeConcern errors.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {assertWriteConcernError, runCommandCheckAdmin} from "jstests/libs/write_concern_util.js";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

let replTest = new ReplSetTest({name: "UserManagementWCSet", nodes: 3, settings: {chainingAllowed: false}});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
let dbName = "user-management-wc-test";
var db = primary.getDB(dbName);
let adminDB = primary.getDB("admin");

function dropUsersAndRoles() {
    db.dropUser("username");
    db.dropUser("user1");
    db.dropUser("user2");
}

let commands = [];

commands.push({
    req: {createUser: "username", pwd: "password", roles: jsTest.basicUserRoles},
    setupFunc: function () {},
    confirmFunc: function () {
        assert(db.auth("username", "password"), "auth failed");
        assert(!db.auth("username", "passworda"), "auth should have failed");
    },
    admin: false,
});

commands.push({
    req: {updateUser: "username", pwd: "password2", roles: jsTest.basicUserRoles},
    setupFunc: function () {
        db.runCommand({createUser: "username", pwd: "password", roles: jsTest.basicUserRoles});
    },
    confirmFunc: function () {
        assert(db.auth("username", "password2"), "auth failed");
        assert(!db.auth("username", "password"), "auth should have failed");
    },
    admin: false,
});

commands.push({
    req: {dropUser: "tempUser"},
    setupFunc: function () {
        db.runCommand({createUser: "tempUser", pwd: "password", roles: jsTest.basicUserRoles});
        assert(db.auth("tempUser", "password"), "auth failed");
    },
    confirmFunc: function () {
        assert(!db.auth("tempUser", "password"), "auth should have failed");
    },
    admin: false,
});

commands.push({
    req: {
        _mergeAuthzCollections: 1,
        tempUsersCollection: "admin.tempusers",
        tempRolesCollection: "admin.temproles",
        db: "",
        drop: false,
    },
    setupFunc: function () {
        adminDB.system.users.remove({});
        adminDB.system.roles.remove({});
        adminDB.createUser({user: "lorax", pwd: "pwd", roles: ["read"]});
        adminDB.createRole({role: "role1", roles: ["read"], privileges: []});
        adminDB.system.users.find().forEach(function (doc) {
            adminDB.tempusers.insert(doc);
        });
        adminDB.system.roles.find().forEach(function (doc) {
            adminDB.temproles.insert(doc);
        });
        adminDB.system.users.remove({});
        adminDB.system.roles.remove({});

        assert.eq(0, adminDB.system.users.find().itcount());
        assert.eq(0, adminDB.system.roles.find().itcount());

        db.createUser({user: "lorax2", pwd: "pwd", roles: ["readWrite"]});
        db.createRole({role: "role2", roles: ["readWrite"], privileges: []});

        assert.eq(1, adminDB.system.users.find().itcount());
        assert.eq(1, adminDB.system.roles.find().itcount());
    },
    confirmFunc: function () {
        assert.eq(2, adminDB.system.users.find().itcount());
        assert.eq(2, adminDB.system.roles.find().itcount());
    },
    admin: true,
});

function assertUserManagementWriteConcernError(res) {
    assert.commandFailed(res);
    assert.commandWorkedIgnoringWriteConcernErrors(res);
    assertWriteConcernError(res);
}

function testValidWriteConcern(cmd) {
    cmd.req.writeConcern = {w: "majority", wtimeout: 25000};
    jsTest.log("Testing " + tojson(cmd.req));

    dropUsersAndRoles();
    cmd.setupFunc();
    let res = runCommandCheckAdmin(db, cmd);
    assert.commandWorked(res);
    assert(!res.writeConcernError, "command on a full replicaset had writeConcernError: " + tojson(res));
    cmd.confirmFunc();
}

function testInvalidWriteConcern(cmd) {
    cmd.req.writeConcern = {w: 15};
    jsTest.log("Testing " + tojson(cmd.req));

    dropUsersAndRoles();
    cmd.setupFunc();
    let res = runCommandCheckAdmin(db, cmd);
    assertUserManagementWriteConcernError(res);
    cmd.confirmFunc();
}

commands.forEach(function (cmd) {
    testValidWriteConcern(cmd);
    testInvalidWriteConcern(cmd);
});

replTest.stopSet();
