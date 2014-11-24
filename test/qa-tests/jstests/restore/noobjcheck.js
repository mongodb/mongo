(function() {

    // Tests that --noobjcheck prevents corrupted bson objects from causing 
    // an error.
    
    jsTest.log('Testing mongorestore with --noobjcheck');

    var toolTest = new ToolTest('noobjcheck');
    toolTest.startDB('foo');

    // this test uses the dump_with_malformed/dbOne/malformed_pass_noobjcheck.bson
    // file. it contains a single bson object with an embedded document, with the
    // length field of the embedded document set to the wrong length (specifically,
    // a length too large which will cause it to overrun the full document). this
    // is a case which is not valid bson but is not corrupt enough that it cannot
    // be read in at all.
    // we also need to pass a write concern of 0 so that the server does not
    // send back an error and cause mongorestore to error out.
    var ret = toolTest.runTool('restore', '--db', 'dbOne',
        '--collection', 'malformed_pass_noobjcheck',
        '--noobjcheck', '--w', '0',
        'jstests/restore/testdata/dump_with_malformed/dbOne/malformed_pass_noobjcheck.bson');
    assert.eq(0, ret);

    toolTest.stop();

}());
