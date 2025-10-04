// Tests the user cache document source

let mongod = MongoRunner.runMongod({auth: ""});
var db = mongod.getDB("admin");
db.createUser({user: "root", pwd: "root", roles: ["userAdminAnyDatabase"]});
db.auth("root", "root");
db.createUser({user: "readOnlyUser", pwd: "foobar", roles: ["readAnyDatabase"]});
let readUserCache = function () {
    let ret = db.aggregate([{$listCachedAndActiveUsers: {}}, {$sort: {"active": -1}}]).toArray();
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

let awaitShell = startParallelShell(function () {
    assert.eq(db.getSiblingDB("admin").auth("readOnlyUser", "foobar"), 1);
}, mongod.port);

const expectedReadOnlyInactive = [
    {username: "root", db: "admin", active: true},
    {username: "readOnlyUser", db: "admin", active: false},
];
assert.soon(function () {
    return friendlyEqual(expectedReadOnlyInactive, readUserCache());
});

MongoRunner.stopMongod(mongod);
awaitShell({checkExitSuccess: false});
