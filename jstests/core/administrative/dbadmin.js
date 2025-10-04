// @tags: [
//    # Assert on the isWritablePrimary field of a hello response. If a primary steps down after
//    # accepting a hello command and returns before its connection is closed, the response can
//    # contain isWritablePrimary: false.
//    does_not_support_stepdowns,
// ]

let t = db.dbadmin;
t.save({x: 1});
t.save({x: 1});

var res = db.adminCommand("listDatabases");
assert(res.databases && res.databases.length > 0, "listDatabases: " + tojson(res));

var res = db.adminCommand({listDatabases: 1, nameOnly: true});
assert(
    res.databases && res.databases.length > 0 && res.totalSize === undefined,
    "listDatabases nameOnly: " + tojson(res),
);

let now = new Date();
let x = db._adminCommand("hello");
assert(x.isWritablePrimary, "hello failed: " + tojson(x));
assert(x.localTime, "hello didn't include time: " + tojson(x));

let localTimeSkew = x.localTime - now;
if (localTimeSkew >= 50) {
    print("Warning: localTimeSkew " + localTimeSkew + " > 50ms.");
}
assert.lt(localTimeSkew, 60 * 60 * 1000 /* one minute */, "hello.localTime");

let before = db.runCommand("serverStatus");
print(before.uptimeMillis);
sleep(100);

let after = db.runCommand("serverStatus");
print(after.uptimeMillis);
assert.gte(after.uptimeMillis, before.uptimeMillis, "uptime estimate should be non-decreasing");
