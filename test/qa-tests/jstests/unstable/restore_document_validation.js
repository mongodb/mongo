(function() {

    if (typeof getToolTest === 'undefined') {
        load('jstests/configs/plain_28.config.js');
    }

    jsTest.log('Testing that restore reacts well to document validation');

    var toolTest = getToolTest('document_validation');
    var commonToolArgs = getCommonToolArguments();

    // where we'll put the dump
    var dumpTarget = 'doc_validation';
    resetDbpath(dumpTarget);

    // the db we will use
    var testDB = toolTest.db.getSiblingDB('test');

    // crate 1000 documents, half of which will pass the validation
    for (var i = 0; i < 1000; i++) {
        if (i%2==0) {
            testDB.bar.insert({ _id: i, num: i+1, s: ''+i });
        } else {
            testDB.bar.insert({ _id: i, num: i+1, s: ''+i, baz: i });
        }
    }
    // sanity check the insertion worked
    assert.eq(1000, testDB.bar.count());

    ret = toolTest.runTool.apply(
            toolTest,
            ['dump','-v'].
                concat(getDumpTarget(dumpTarget)).
                concat(commonToolArgs)
    );
    assert.eq(0, ret,"the dump runs successfully");

    testDB.dropDatabase();
    assert.eq(0, testDB.bar.count(),"after the drop, the documents not seen");

    // sanity check that we can restore the data without validation
    ret = toolTest.runTool.apply(
            toolTest,
            ['restore'].
                concat(getRestoreTarget(dumpTarget)).
                concat(commonToolArgs)
    );
    assert.eq(0, ret);

    assert.eq(1000, testDB.bar.count(),"after the restore, the documents are seen again");

    testDB.dropDatabase();
    assert.eq(0, testDB.bar.count(),"after the drop, the documents not seen");

    // turn on validation
    r = testDB.createCollection("bar",{validator:{baz:{$exists:true}}});
    assert.eq(r,{ok:1},"create collection with validation works");

    // test that it's working
    r = testDB.bar.insert({num:10000});
    assert.eq(r.nInserted,0,"invalid documents can't be inserted")

    // restore the 1000 records of which only 500 are valid
    ret = toolTest.runTool.apply(
            toolTest,
            ['restore','-v'].
                concat(getRestoreTarget(dumpTarget)).
                concat(commonToolArgs)
    );
    assert.eq(0, ret,"restore against a collection with validation on still succeeds");

    assert.eq(500, testDB.bar.count(),"only the valid documents are restored");

    jsTest.log('Testing that dump and restore the validation rules themselves');

    // clear out the database, including the validation rules
    testDB.dropDatabase();
    assert.eq(0, testDB.bar.count(),"after the drop, the documents not seen");

    // test that we can insert an "invalid" document
    r = testDB.bar.insert({num:10000});
    assert.eq(r.nInserted,1,"invalid documents can be inserted")

    testDB.dropDatabase();
    assert.eq(0, testDB.bar.count(),"after the drop, the documents not seen");

    // restore the 1000 records again
    ret = toolTest.runTool.apply(
            toolTest,
            ['restore'].
                concat(getRestoreTarget(dumpTarget)).
                concat(commonToolArgs)
    );
    assert.eq(0, ret);
    assert.eq(1000, testDB.bar.count());

    // turn on validation on a existing collection
    testDB.runCommand({"collMod": "bar", "validator" : {baz: {$exists: true}}});

    // re-dump everything, this time dumping the validation rules themselves
    ret = toolTest.runTool.apply(
            toolTest,
            ['dump','-v'].
                concat(getDumpTarget(dumpTarget)).
                concat(commonToolArgs)
    );
    assert.eq(0, ret,"the dump runs successfully");

    // clear out the database, including the validation rules
    testDB.dropDatabase();
    assert.eq(0, testDB.bar.count(),"after the drop, the documents not seen");

    // test that we can insert an "invalid" document
    r = testDB.bar.insert({num:10000});
    assert.eq(r.nInserted,1,"invalid documents can be inserted")

    testDB.dropDatabase();
    assert.eq(0, testDB.bar.count(),"after the drop, the documents not seen");

    // restore the 1000 records again
    ret = toolTest.runTool.apply(
            toolTest,
            ['restore'].
                concat(getRestoreTarget(dumpTarget)).
                concat(commonToolArgs)
    );
    assert.eq(0, ret,"restoring rules and some invalid documents runs");
    assert.eq(500, testDB.bar.count(),"restore the validation rules and the valid documents");

}());
