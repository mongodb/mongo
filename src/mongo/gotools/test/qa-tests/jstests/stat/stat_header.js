(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load('jstests/libs/mongostat.js');
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var toolTest = getToolTest('stat_header');

  function outputIncludesHeader() {
    return rawMongoProgramOutput()
      .split("\n").some(function(line) {
        return line.match(/^sh\d+\| insert/);
      });
  }

  clearRawMongoProgramOutput();
  x = runMongoProgram("mongostat", "--port", toolTest.port, "--rowcount", 1);
  assert.soon(outputIncludesHeader, "normally a header appears");

  clearRawMongoProgramOutput();
  x = runMongoProgram("mongostat", "--port", toolTest.port, "--rowcount", 1, "--noheaders");
  assert.eq.soon(false, outputIncludesHeader, "--noheaders suppresses the header");

  toolTest.stop();
}());
