(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load("jstests/libs/mongostat.js");
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;
  var commonToolArgs = getCommonToolArguments();
  print("common tool sargs");
  printjson(commonToolArgs);

  var toolTest = getToolTest('stat_rowcount');
  var x, pid;
  clearRawMongoProgramOutput();

  x = runMongoProgram("mongostat", "--host", toolTest.m.host, "--rowcount", 7, "--noheaders");
  assert.eq.soon(7, function() {
    return rawMongoProgramOutput().split("\n").filter(function(r) {
      return r.match(rowRegex);
    }).length;
  }, "--rowcount value is respected correctly");

  startTime = new Date();
  x = runMongoProgram("mongostat", "--host", toolTest.m.host, "--rowcount", 3, "--noheaders", 3);
  endTime = new Date();
  duration = Math.floor((endTime - startTime) / 1000);
  assert.gte(duration, 9, "sleep time affects the total time to produce a number or results");

  clearRawMongoProgramOutput();

  pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--port", toolTest.port].concat(commonToolArgs));
  assert.strContains.soon('sh'+pid+'|  ', rawMongoProgramOutput, "should produce some output");
  assert.eq(exitCodeStopped, stopMongoProgramByPid(pid), "stopping should cause mongostat exit with a 'stopped' code");

  x = runMongoProgram.apply(null, ["mongostat", "--port", toolTest.port - 1, "--rowcount", 1].concat(commonToolArgs));
  assert.neq(exitCodeSuccess, x, "can't connect causes an error exit code");

  x = runMongoProgram.apply(null, ["mongostat", "--rowcount", "-1"].concat(commonToolArgs));
  assert.eq(exitCodeBadOptions, x, "mongostat --rowcount specified with bad input: negative value");

  x = runMongoProgram.apply(null, ["mongostat", "--rowcount", "foobar"].concat(commonToolArgs));
  assert.eq(exitCodeBadOptions, x, "mongostat --rowcount specified with bad input: non-numeric value");

  x = runMongoProgram.apply(null, ["mongostat", "--host", "badreplset/127.0.0.1:" + toolTest.port, "--rowcount", 1].concat(commonToolArgs));
  assert.eq(exitCodeErr, x, "--host used with a replica set string for nodes not in a replica set");

  pid = startMongoProgramNoConnect.apply(null, ["mongostat", "--host", "127.0.0.1:" + toolTest.port].concat(commonToolArgs));
  assert.strContains.soon('sh'+pid+'|  ', rawMongoProgramOutput, "should produce some output");

  MongoRunner.stopMongod(toolTest.port);
  assert.gte.soon(10, function() {
    var rows = statRows();
    return statFields(rows[rows.length - 1]).length;
  }, "should stop showing new stat lines");
  assert.eq(exitCodeStopped, stopMongoProgramByPid(pid), "mongostat shouldn't error out when the server goes down");
}());
