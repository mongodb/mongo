(function() {

load("jstests/libs/mongostat.js");

port = allocatePorts(1);

baseName = "tool_stat1";

m = startMongod("--port", port[0], "--dbpath", MongoRunner.dataPath + baseName + port[0], "--nohttpinterface", "--bind_ip", "127.0.0.1");

pid = startMongoProgramNoConnect("mongostat", "--port", port[0]);

sleep(100);

assert.eq(exitCodeStopped, stopMongoProgramByPid(pid), "stopping should cause mongostat exit 2");

pid = startMongoProgramNoConnect("mongostat", "--port", port[0], "--rowcount", 1);

sleep(1100);

assert.eq(exitCodeSuccess, stopMongoProgramByPid(pid), "a successful run exits 0");

x = runMongoProgram("mongostat", "--port", port[0] - 1, "--rowcount", 1);

assert.eq(exitCodeErr, x, "can't connect causes an error exit code");

pid = startMongoProgramNoConnect("mongostat", "--rowcount", "-1")

sleep(100);

assert.eq(exitCodeErr, stopMongoProgramByPid(pid), "mongostat --rowcount specified with bad input: negative value")

pid = startMongoProgramNoConnect("mongostat", "--rowcount", "foobar");

sleep(100);

assert.eq(exitCodeErr, stopMongoProgramByPid(pid), "mongostat --rowcount specified with bad input: non-numeric value");

pid = startMongoProgramNoConnect("mongostat", "--host", "foobar/127.0.0.1:" + port[0]);

sleep(500);

assert.eq(exitCodeErr, stopMongoProgramByPid(pid), "--host used with a replica set string for nodes not in a replica set");

pid = startMongoProgramNoConnect("mongostat", "--host", "127.0.0.1:" + port[0]);

sleep(2000);

print(JSON.stringify(m));

MongoRunner.stopMongod(port[0]);

sleep(7000); // 1 second for the current sleep time, 5 seconds for the connection timeout, 1 second for fuzz

assert.eq(exitCodeStopped, stopMongoProgramByPid(pid), "mongostat shouldn't error out when the server goes down");

}());
