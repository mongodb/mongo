(function() {
  // This test creates a collection with a subdocument _id field. We export the collection,
  // replace the existing documents with a pre-made dataset and --mode=upsert, then overwrite
  // that with the original data, again with --mode=upsert. This verifies that import and
  // export do not change the order of _id fields.
  jsTest.log('Testing running import with --mode=upsert and _id subdocuments');

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('import');
  var db1 = toolTest.db;
  var commonToolArgs = getCommonToolArguments();

  var db = db1.getSiblingDB("upserttest");
  db.dropDatabase();

  // create a set of documents with a subdocument _id
  var h, i, j;
  for (h = 0; h < 2; h++) {
    var data = [];
    for (i = h * 50; i < (h+1) * 50; i++) {
      for (j = 0; j < 20; j++) {
        data.push({
          _id: {
            a: i,
            b: [0, 1, 2, {c: j, d: "foo"}],
            e: "bar",
          },
          x: "string",
        });
      }
    }
    db.c.insertMany(data);
  }
  assert.eq(db.c.count(), 2000);

  jsTest.log('Exporting documents with subdocument _ids.');
  var ret = toolTest.runTool.apply(toolTest, ["export",
      "-o", toolTest.extFile,
      "--db", db.getName(),
      "--collection", db.c.getName()]
    .concat(commonToolArgs));
  assert.eq(ret, 0, "export should succeed");

  jsTest.log('Upserting pre-made documents with subdocument _ids.');
  ret = toolTest.runTool.apply(toolTest, ["import",
      "--file", "jstests/import/testdata/upsert3.json",
      "--mode=upsert",
      "--db", db.getName(),
      "--collection", db.c.getName()]
    .concat(commonToolArgs));
  assert.eq(ret, 0, "import should succeed");
  assert.eq(db.c.count(), 2000,
      "count should be the same before and after import");

  // check each document
  for (i = 0; i < 100; i++) {
    for (j = 0; j < 20; j++) {
      assert.eq(db.c.findOne({_id: {a: i, b: [0, 1, 2, {c: j, d: "foo"}], e: "bar"}}).x, "str2",
          "all documents should be updated");
    }
  }

  jsTest.log('Upserting original exported documents with subdocument _ids.');
  ret = toolTest.runTool.apply(toolTest, ["import",
      "--file", toolTest.extFile,
      "--mode=upsert",
      "--db", db.getName(),
      "--collection", db.c.getName()]
    .concat(commonToolArgs));
  assert.eq(ret, 0, "import should succeed");
  assert.eq(db.c.count(), 2000,
      "count should be the same before and after import");

  // check each document to see that it is back at its original value
  for (i = 0; i < 100; i++) {
    for (j = 0; j < 20; j++) {
      assert.eq(db.c.findOne({_id: {a: i, b: [0, 1, 2, {c: j, d: "foo"}], e: "bar"}}).x, "string",
          "all documents should be updated");
    }
  }

  toolTest.stop();
}());
