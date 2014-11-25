(function() {

    // Write a test
    var toolTest = new ToolTest('stop_on_error');
    toolTest.startDB('foo');

    var dbOne = toolTest.db.getSiblingDB('dbOne');
    // create a test collection
    for(var i=0;i<=100;i++){
      dbOne.test.insert({_id:i, x:i*i})
    }


    // dump it
    var dumpTarget = 'stop_on_error_dump';
    var ret = toolTest.runTool('dump', '--out', dumpTarget);
    assert.eq(0, ret);

    // drop the database so it's empty
    dbOne.dropDatabase()

    // restore it - database was just dropped, so this should work successfully
    ret = toolTest.runTool('restore', dumpTarget);
    assert.eq(0, ret, "restore to empty DB should have returned successfully");

    // restore it again with --stopOnError - this one should fail since there are dup keys
    ret = toolTest.runTool('restore', dumpTarget, "--stopOnError", "-vvvv");
    assert.neq(0, ret);

    // restore it one more time without --stopOnError - there are dup keys but they will be ignored
    ret = toolTest.runTool('restore', dumpTarget, "-vvvv");
    assert.eq(0, ret);

    // success
    toolTest.stop();

}());
