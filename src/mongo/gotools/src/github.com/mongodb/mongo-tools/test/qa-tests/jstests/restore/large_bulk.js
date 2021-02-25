(function() {

  // this test tests that the bulk api doesn't create BSON documents greater then the
  // 16MB limit, as was discovered in TOOLS-939.

  if (typeof getToolTest === 'undefined') {
    load('jstests/configs/plain_28.config.js');
  }

  var toolTest = getToolTest('large_bulk');
  var commonToolArgs = getCommonToolArguments();

  var dbOne = toolTest.db.getSiblingDB('dbOne');
  // create a test collection

  var oneK="";
  var oneM="";
  var i;
  for (i=0; i<=1024; i++) {
    oneK+="X";
  }
  for (i=0; i<=1024; i++) {
    oneM+=oneK;
  }

  var data = [];
  for (i=0; i<=32; i++) {
    data.push({data: oneM});
  }
  dbOne.test.insertMany(data);

  // dump it
  var dumpTarget = 'large_bulk_dump';
  resetDbpath(dumpTarget);
  var ret = toolTest.runTool.apply(toolTest, ['dump']
    .concat(getDumpTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret);

  // drop the database so it's empty
  dbOne.dropDatabase();

  // restore it
  // 32 records are well under the 1k batch size
  // so this should test wether the physcial size limit is respected
  ret = toolTest.runTool.apply(toolTest, ['restore']
    .concat(getRestoreTarget(dumpTarget))
    .concat(commonToolArgs));
  assert.eq(0, ret, "restore to empty DB should have returned successfully");

  // success
  toolTest.stop();

}());
