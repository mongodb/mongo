(function() {

load("jstests/libs/mongostat.js");

port = allocatePorts(1);

baseName = "stat_auth";

m = startMongod("--auth", "--port", port[0], "--dbpath", MongoRunner.dataPath + baseName + port[0], "--nohttpinterface", "--bind_ip", "127.0.0.1");

db = m.getDB("admin");

db.createUser({
    user: "foobar",
    pwd: "foobar",
    roles: jsTest.adminUserRoles
});

assert(db.auth("foobar", "foobar"), "auth failed");

x = runMongoProgram("mongostat", "--host", "127.0.0.1:" + port[0], "--username", "foobar", "--password", "foobar", "--rowcount", "1", "--authenticationDatabase", "admin");

assert.eq(x, 0, "mongostat should exit successfully with foobar:foobar");

x = runMongoProgram("mongostat", "--host", "127.0.0.1:" + port[0], "--username", "foobar", "--password", "wrong", "--rowcount", "1", "--authenticationDatabase", "admin");

assert.eq(x, exitCodeErr, "mongostat should exit with an error exit code with foobar:wrong");

}());
