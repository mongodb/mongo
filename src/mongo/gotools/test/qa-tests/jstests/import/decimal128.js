(function() {
  // skip this test where NumberDecimal is unsupported (3.2 and earlier)
  if (typeof NumberDecimal === 'undefined') {
    return;
  }

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  jsTest.log('Testing running import with various data types');

  var toolTest = getToolTest('import');
  var db1 = toolTest.db;
  var commonToolArgs = getCommonToolArguments();

  var testDoc = {
    _id: "foo",
    x: NumberDecimal("124124"),
  };

  // Make a dummy file to import by writing a test collection and exporting it
  assert.eq(0, db1.c.count(), "initial collection is not empty");
  db1.c.save(testDoc);
  toolTest.runTool.apply(toolTest, ["export",
      "--out", toolTest.extFile,
      "-d", toolTest.baseName,
      "-c", db1.c.getName()]
      .concat(commonToolArgs));

  toolTest.runTool.apply(toolTest, ["import",
      "--file", toolTest.extFile,
      "--db", "imported",
      "--collection", "dec128"]
      .concat(commonToolArgs));
  var importedDocs = db1.c.getDB().getSiblingDB("imported").dec128.find().toArray();

  assert.eq(importedDocs.length, 1, "incorrect # of docs imported");

  var importedDoc = importedDocs[0];

  assert.eq(importedDoc, testDoc, "imported doc and test doc do not match");

  toolTest.stop();
}());
