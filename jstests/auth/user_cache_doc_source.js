// Tests the user cache document source

(function() {
'use strict';

var mongod = MongoRunner.runMongod({auth: ""});
var db = mongod.getDB("admin");
db.createUser({user: "root", pwd: "root", roles: ["userAdminAnyDatabase"]});
db.auth("root", "root");
db.createUser({user: "readOnlyUser", pwd: "foobar", roles: ["readAnyDatabase"]});
var readUserCache = function() {
    var ret = db.aggregate([{$listCachedAndActiveUsers: {}}]).toArray();
    print(tojson(ret));
    return ret;
};

const expectedOnlyRoot = [{username: "root", db: "admin", active: true}];
assert.eq(expectedOnlyRoot, readUserCache());

/* This is broken because of SERVER-36384
var newConn = new Mongo(mongod.name);
assert.eq(newConn.getDB("admin").auth("readOnlyUser", "foobar"), 1);

const expectedBothActive = [
    { username: "root", db: "admin", active: true },
    { username: "readOnlyUser", db: "admin", active: true }
];
assert.eq(expectedBothActive, readUserCache());

newConn.close();
*/

var awaitShell = startParallelShell(function() {
    assert.eq(db.getSisterDB("admin").auth("readOnlyUser", "foobar"), 1);
}, mongod.port);

const expectedReadOnlyInactive = [
    {username: "readOnlyUser", db: "admin", active: false},
    {username: "root", db: "admin", active: true}
];
assert.soon(function() {
    return friendlyEqual(expectedReadOnlyInactive, readUserCache());
});

MongoRunner.stopMongod(mongod);
awaitShell({checkExitSuccess: false});
})();
