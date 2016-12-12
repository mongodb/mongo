(function() {
  load("jstests/libs/mongostat.js");
  var port = allocatePort();
  var m = startMongod(
    "--auth",
    "--port", port,
    "--dbpath", MongoRunner.dataPath+"stat_auth"+port,
    "--nohttpinterface",
    "--bind_ip", "127.0.0.1");

  var db = m.getDB("admin");
  db.createUser({
    user: "foobar",
    pwd: "foobar",
    roles: jsTest.adminUserRoles
  });

  assert(db.auth("foobar", "foobar"), "auth failed");

  var args = ["mongostat",
    "--host", "127.0.0.1:" + port,
    "--rowcount", "1",
    "--authenticationDatabase", "admin",
    "--username", "foobar"];

  var x = runMongoProgram.apply(null, args.concat("--password", "foobar"));
  assert.eq(x, exitCodeSuccess, "mongostat should exit successfully with foobar:foobar");

  x = runMongoProgram.apply(null, args.concat("--password", "wrong"));
  assert.eq(x, exitCodeErr, "mongostat should exit with an error exit code with foobar:wrong");
}());
