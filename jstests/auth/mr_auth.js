// MapReduce executed by a read-only user when --auth enabled should only be able to use inline
// mode. Other modes require writing to an output collection which is not allowed. SERVER-3345
//
// This test requires users to persist across a restart.
// @tags: [requires_persistence]

baseName = "jstests_mr_auth";
dbName = "test";
out = baseName + "_out";

map = function() {
    emit(this.x, this.y);
};
red = function(k, vs) {
    var s = 0;
    for (var i = 0; i < vs.length; i++)
        s += vs[i];
    return s;
};
red2 = function(k, vs) {
    return 42;
};

// make sure writing is allowed when started without --auth enabled

dbms = MongoRunner.runMongod({bind_ip: "127.0.0.1"});
var d = dbms.getDB(dbName);
var t = d[baseName];

for (var i = 0; i < 1000; i++)
    t.insert({_id: i, x: i % 10, y: i % 100});
assert.eq(1000, t.count(), "inserts failed");

d.dropAllUsers();
d.getSisterDB("admin").createUser({user: "admin", pwd: "admin", roles: jsTest.adminUserRoles});
d.getSisterDB("admin").auth('admin', 'admin');
d.createUser({user: "write", pwd: "write", roles: jsTest.basicUserRoles});
d.createUser({user: "read", pwd: "read", roles: jsTest.readOnlyUserRoles});
d.getSisterDB("admin").logout();

t.mapReduce(map, red, {out: {inline: 1}});

t.mapReduce(map, red, {out: {replace: out}});
t.mapReduce(map, red, {out: {reduce: out}});
t.mapReduce(map, red, {out: {merge: out}});

d[out].drop();

MongoRunner.stopMongod(dbms);

// In --auth mode, read-only user should not be able to write to existing or temporary collection,
// thus only can execute inline mode

dbms = MongoRunner.runMongod(
    {restart: true, cleanData: false, dbpath: dbms.dbpath, auth: "", bind_ip: "127.0.0.1"});
d = dbms.getDB(dbName);
t = d[baseName];

assert.throws(function() {
    t.findOne();
}, [], "read without login");

assert.throws(function() {
    t.mapReduce(map, red, {out: {inline: 1}});
}, [], "m/r without login");

d.auth("read", "read");

t.findOne();

t.mapReduce(map, red, {out: {inline: 1}});

assert.throws(function() {
    t.mapReduce(map, red2, {out: {replace: out}});
}, [], "read-only user shouldn't be able to output m/r to a collection");
assert.throws(function() {
    t.mapReduce(map, red2, {out: {reduce: out}});
}, [], "read-only user shouldn't be able to output m/r to a collection");
assert.throws(function() {
    t.mapReduce(map, red2, {out: {merge: out}});
}, [], "read-only user shouldn't be able to output m/r to a collection");

assert.eq(0, d[out].count(), "output collection should be empty");

d.logout();

assert.throws(function() {
    t.mapReduce(map, red, {out: {replace: out}});
}, [], "m/r without login");

d.auth("write", "write");

t.mapReduce(map, red, {out: {inline: 1}});

t.mapReduce(map, red, {out: {replace: out}});
t.mapReduce(map, red, {out: {reduce: out}});
t.mapReduce(map, red, {out: {merge: out}});

// make sure it fails if output to a diff db
assert.throws(function() {
    t.mapReduce(map, red, {out: {replace: out, db: "admin"}});
});

MongoRunner.stopMongod(dbms);

print("\n\n\nmr_auth.js SUCCESS\n\n\n");
