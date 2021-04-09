(function() {

'use strict';

const conn = MongoRunner.runMongod({auth: ""});

const admin = conn.getDB("admin");
const errorCodeUnauthorized = 13;

admin.createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});

print("make sure curop, killop, and unlock fail");

let x = admin.currentOp();
assert(!("inprog" in x), tojson(x));
assert.eq(x.code, errorCodeUnauthorized, tojson(x));

x = admin.killOp(123);
assert(!("info" in x), tojson(x));
assert.eq(x.code, errorCodeUnauthorized, tojson(x));

x = admin.fsyncUnlock();
assert(x.errmsg != "fsyncUnlock called when not locked", tojson(x));
assert.eq(x.code, errorCodeUnauthorized, tojson(x));

conn.getDB("admin").auth("foo", "bar");

assert("inprog" in admin.currentOp());
assert("info" in admin.killOp(123));
assert.eq(admin.fsyncUnlock().errmsg, "fsyncUnlock called when not locked");

MongoRunner.stopMongod(conn, null, {user: "foo", pwd: "bar"});
})();
