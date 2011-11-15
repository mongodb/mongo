var conn = MongoRunner.runMongod({auth : "", port : 31001});

var admin = conn.getDB("admin");

admin.addUser("foo","bar");

print("make sure curop, killop, and unlock fail");

var x = admin.$cmd.sys.inprog.findOne();
assert(!("inprog" in x), tojson(x));
assert.eq(x.err, "unauthorized", tojson(x));

x = admin.killOp(123);
assert(!("info" in x), tojson(x));
assert.eq(x.err, "unauthorized", tojson(x));

x = admin.fsyncUnlock();
assert(x.errmsg != "not locked", tojson(x));
assert.eq(x.err, "unauthorized", tojson(x));

conn.getDB("admin").auth("foo","bar");

assert("inprog" in admin.currentOp());
assert("info" in admin.killOp(123));
assert.eq(admin.fsyncUnlock().errmsg, "not locked");
