(function() {

if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
    print("common tool sargs")
    printjson( getCommonToolArguments())
}

load("jstests/libs/mongostat.js");
    print("common tool sargs 2")
var commonToolArgs = getCommonToolArguments()
    printjson(commonToolArgs)

port = allocatePorts(1);

baseName = "tool_stat1";

m = startMongod("--port", port[0], "--dbpath", MongoRunner.dataPath + baseName + port[0], "--nohttpinterface", "--bind_ip", "127.0.0.1");

pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--port", port[0]].concat(commonToolArgs));

sleep(1000);

// FIXME currently, on windows, stopMongoProgramByPid doesn't terminiate a process in a way that it can control it's exit code
// so the return of stopMongoProgramByPid will probably be 1 in either case.
assert.eq(_isWindows() ? 1 : exitCodeStopped, stopMongoProgramByPid(pid), "stopping should cause mongostat exit with a 'stopped' code");

pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--port", port[0]].concat(commonToolArgs));


sleep(1100);

assert.eq(_isWindows() ? 1 : exitCodeStopped, stopMongoProgramByPid(pid), "stopping should cause mongostat exit with a 'stopped' code");

x = startMongoProgramNoConnect.apply(null, ["mongostat", "--port", port[0] - 1, "--rowcount", 1].concat(commonToolArgs));

assert.neq(exitCodeSuccess, x, "can't connect causes an error exit code");

pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--rowcount", "-1"].concat(commonToolArgs))

sleep(200);

assert.eq(exitCodeBadOptions, stopMongoProgramByPid(pid), "mongostat --rowcount specified with bad input: negative value")

pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--rowcount", "foobar"].concat(commonToolArgs));

sleep(100);

assert.eq(exitCodeBadOptions, stopMongoProgramByPid(pid), "mongostat --rowcount specified with bad input: non-numeric value");

pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--host", "badreplset/127.0.0.1:" + port[0], "--rowcount", 1].concat(commonToolArgs));

sleep(5000);

assert.eq(exitCodeErr, stopMongoProgramByPid(pid), "--host used with a replica set string for nodes not in a replica set");

pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--host", "127.0.0.1:" + port[0]].concat(commonToolArgs));

sleep(2000);

print(JSON.stringify(m));

MongoRunner.stopMongod(port[0]);

sleep(7000); // 1 second for the current sleep time, 5 seconds for the connection timeout, 1 second for fuzz

assert.eq(_isWindows() ? 1 : exitCodeStopped, stopMongoProgramByPid(pid), "mongostat shouldn't error out when the server goes down");

}());
