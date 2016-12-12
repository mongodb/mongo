(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  load("jstests/libs/mongostat.js");
  load("jstests/libs/extended_assert.js");
  var assert = extendedAssert;

  var toolTest = getToolTest("stat_custom_headers");
  var port = toolTest.port;

  var x, rows;
  x = runMongoProgram("mongostat", "--port", port,
      "-o", "host,conn,time", "-O", "metrics.record.moves");
  assert.eq(x, exitCodeBadOptions, "mongostat should fail with both -o and -O options");
  clearRawMongoProgramOutput();

  // basic -o --humanReadable=false
  x = runMongoProgram("mongostat", "--port", port,
      "-o", "host,conn,time", "-n", 4, "--humanReadable=false");
  assert.eq(x, 0, "mongostat should succeed with -o and -n options");
  assert.eq.soon(5, function() {
    rows = statRows();
    return rows.length;
  }, "expected 5 rows in mongostat output");
  assert.eq(statFields(rows[0]).join(), "host,conn,time",
      "first row doesn't match 'host conn time'");
  assert.eq(statFields(rows[1]).length, 3,
      "there should be exactly three entries for a row of this stat output");
  clearRawMongoProgramOutput();

  // basic -o
  x = runMongoProgram("mongostat", "--port", port,
      "-o", "host,conn,time", "-n", 4);
  assert.eq(x, 0, "mongostat should succeed with -o and -n options");
  assert.eq.soon(5, function() {
    rows = statRows();
    return rows.length;
  }, "expected 5 rows in mongostat output");
  assert.eq(statFields(rows[0]).join(), "host,conn,time",
      "first row doesn't match 'host conn time'");
  assert.eq(statFields(rows[1]).length, 5,
      "there should be exactly five entries for a row of this stat output (time counts as three)");
  clearRawMongoProgramOutput();

  // basic -O
  x = runMongoProgram("mongostat", "--port", port,
      "-O", "host", "-n", 4);
  assert.eq(x, 0, "mongostat should succeed with -o and -n options");
  rows = statRows();
  var fields = statFields(rows[0]);
  assert.eq(fields[fields.length-1], "host",
      "first row should end with added 'host' field");
  clearRawMongoProgramOutput();

  // named
  x = runMongoProgram("mongostat", "--port", port,
      "-o", "host=H,conn=C,time=MYTiME", "-n", 4);
  assert.eq(x, 0, "mongostat should succeed with -o and -n options");
  assert.eq.soon(5, function() {
    rows = statRows();
    return rows.length;
  }, "expected 5 rows in mongostat output");
  assert.eq(statFields(rows[0]).join(), "H,C,MYTiME",
      "first row doesn't match 'H C MYTiME'");
  assert.eq(statFields(rows[1]).length, 5,
      "there should be exactly five entries for a row of this stat output (time counts as three)");
  clearRawMongoProgramOutput();

  // serverStatus custom field
  x = runMongoProgram("mongostat", "--port", port,
      "-o", "host,conn,mem.bits", "-n", 4);
  assert.eq(x, 0, "mongostat should succeed with -o and -n options");
  assert.eq.soon(5, function() {
    rows = statRows();
    return rows.length;
  }, "expected 5 rows in mongostat output");
  assert.eq(statFields(rows[0]).join(), "host,conn,mem.bits",
      "first row doesn't match 'host time mem.bits'");
  fields = statFields(rows[1]);
  assert.eq(fields.length, 3,
      "there should be exactly three entries for a row of this stat output");
  assert(fields[2] === "32" || fields[2] === "64",
      "mem.bits didn't yield valid output (should be one of 32 or 64, was '"
      +fields[2]+"')");
  clearRawMongoProgramOutput();

  // serverStatus named field
  x = runMongoProgram("mongostat", "--port", port,
      "-o", "host,conn=MYCoNN,mem.bits=BiTs", "-n", 4);
  assert.eq(x, 0, "mongostat should succeed with -o and -n options");
  assert.eq.soon(5, function() {
    rows = statRows();
    return rows.length;
  }, "expected 5 rows in mongostat output");
  assert.eq(statFields(rows[0]).join(), "host,MYCoNN,BiTs",
      "first row doesn't match 'host MYTiME BiTs'");
  fields = statFields(rows[1]);
  assert.eq(fields.length, 3,
      "there should be exactly three entries for a row of this stat output");
  assert(fields[2] === "32" || fields[2] === "64",
      "mem.bits didn't yield valid output (should be one of 32 or 64, was '"
      +fields[2]+"')");
  clearRawMongoProgramOutput();

  toolTest.stop();
}());
