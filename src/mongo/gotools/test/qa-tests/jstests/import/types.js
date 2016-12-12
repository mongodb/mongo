(function() {
  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }
  jsTest.log('Testing running import with various data types');

  var toolTest = getToolTest('import');
  var db1 = toolTest.db;
  var commonToolArgs = getCommonToolArguments();

  var testDoc = {
    _id: ObjectId(),
    a: BinData(0, "e8MEnzZoFyMmD7WSHdNrFJyEk8M="),
    b: Boolean(1),
    d: "this is a string",
    e: ["this is an ", 2, 23.5, "array with various types in it"],
    f: {"this is": "an embedded doc"},
    g: function () {
      print("hey sup");
    },
    h: null,
    i: true,
    j: false,
    k: NumberLong(10000),
    l: MinKey(),
    m: MaxKey(),
    n: ISODate("2015-02-25T16:42:11Z"),
    o: DBRef('namespace', 'identifier', 'database'),
    p: NumberInt(5),
    q: 5.0,
  };

  // Make a dummy file to import by writing a test collection and exporting it
  assert.eq(0, db1.c.count(), "setup1");
  db1.c.save(testDoc);
  toolTest.runTool.apply(toolTest, ["export",
      "--out", toolTest.extFile,
      "-d", toolTest.baseName,
      "-c", db1.c.getName()]
    .concat(commonToolArgs));

  toolTest.runTool.apply(toolTest, ["import",
      "--file", toolTest.extFile,
      "--db", "imported",
      "--collection", "testcoll2"]
    .concat(commonToolArgs));
  var postImportDoc = db1.c.getDB().getSiblingDB("imported").testcoll2.findOne();

  printjson(postImportDoc);

  for (var docKey in testDoc) {
    if (!testDoc.hasOwnProperty(docKey)) {
      continue;
    }
    jsTest.log("checking field " + docKey);
    if (typeof testDoc[docKey] === 'function') {
      // SERVER-23472: As of 3.3.5, JS functions are serialized when inserted,
      // so accept either the original function or its serialization
      try {
        assert.eq(testDoc[docKey], postImportDoc[docKey],
            "function does not directly match");
      } catch (e) {
        assert.eq({code: String(testDoc[docKey])}, postImportDoc[docKey],
            "serialized function does not match");
      }
      continue;
    }
    assert.eq(testDoc[docKey], postImportDoc[docKey],
        "imported field " + docKey + " does not match original");
  }

  // DBPointer should turn into a DBRef with a $ref field and hte $id field being an ObjectId. It will not convert back to a DBPointer.

  var oid = ObjectId();
  var irregularObjects = {
    _id: ObjectId(),
    a: DBPointer('namespace', oid),
    b: NumberInt("5"),
    c: NumberLong("5000"),
    d: 5,
    e: 9223372036854775,
  };

  db1.c.drop();
  db1.c.getDB().getSiblingDB("imported").testcoll3.drop();
  assert.eq(0, db1.c.count(), "setup1");
  db1.c.save(irregularObjects);
  toolTest.runTool.apply(toolTest, ["export",
      "--out", toolTest.extFile,
      "-d", toolTest.baseName,
      "-c", db1.c.getName()]
    .concat(commonToolArgs));

  toolTest.runTool.apply(toolTest, ["import",
      "--file", toolTest.extFile,
      "--db", "imported",
      "--collection", "testcoll3"]
    .concat(commonToolArgs));
  postImportDoc = db1.c.getDB().getSiblingDB("imported").testcoll3.findOne();

  printjson(postImportDoc);

  var dbRef = DBRef("namespace", oid);
  assert.eq(postImportDoc["a"], dbRef);

  assert.eq(postImportDoc["b"], 5);
  assert.eq(postImportDoc["d"], 5);

  var numLong = NumberLong(5000);
  assert.eq(postImportDoc["c"], numLong);

  numLong = NumberLong(9223372036854775);
  assert.eq(postImportDoc["e"], numLong);


  toolTest.stop();
}());
