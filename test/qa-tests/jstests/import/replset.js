(function() {
    jsTest.log('Testing running import with upserts');

    var toolTest = new ToolTest('import_repl');

    var replset1 = new ReplSetTest({nodes:3, name:"importtest"})
    replset1.startSet()
    replset1.initiate()

    var primary = replset1.getPrimary()
    var secondary = replset1.getURL()

    var db = primary.getDB("import_repl_test")
    var ret = toolTest.runTool("import", "--file",
      "jstests/import/testdata/basic.json",
      "--db", db.getName(),
      "--collection", db.c.getName(), 
      "--host", secondary.host + ":" + secondary.port)

    // trying to write to the secondary should fail
    assert.neq(ret, 0, "writing to secondary should fail")

    // now import using the primary
    var ret = toolTest.runTool("import", "--file",
      "jstests/import/testdata/basic.json",
      "--db", db.getName(),
      "--collection", db.c.getName(), 
      "--host", primary.host + ":" + primary.port)
    assert.eq(ret, 0, "writing to primary should succeed")
    db.dropDatabase()

    // import using the secondary but include replset name, should succeed
    var ret = toolTest.runTool("import", "--file",
      "jstests/import/testdata/basic.json",
      "--db", db.getName(),
      "--collection", db.c.getName(), 
      "--host", replset1.name + "/" + secondary.host + ":" + secondary.port)
    assert.eq(ret, 0, "writing to secondary with replset name should succeed")

    toolTest.stop();
}());
