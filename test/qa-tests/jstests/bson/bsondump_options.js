// This test checks reasonable and unreasonable option configurations for bsondump

(function(){
    var sampleFilepath = "jstests/bson/testdata/sample.bson"
    var x = _runMongoProgram( "bsondump", "--type=fake", sampleFilepath);
    assert.neq(x, 0, "bsondump should exit with failure when given a non-existent type");

    x = _runMongoProgram( "bsondump", "jstests/bson/testdata/asdfasdfasdf");
    assert.neq(x, 0, "bsondump should exit with failure when given a non-existent file");

    x = _runMongoProgram( "bsondump", "--noobjcheck", sampleFilepath);
    assert.neq(x, 0, "bsondump should exit with failure when given --noobjcheck");

    x = _runMongoProgram( "bsondump", "--collection", sampleFilepath);
    assert.neq(x, 0, "bsondump should exit with failure when given --collection");

    x = _runMongoProgram( "bsondump", sampleFilepath, sampleFilepath);
    assert.neq(x, 0, "bsondump should exit with failure when given multiple files");

    x = _runMongoProgram( "bsondump", '--bsonFile', sampleFilepath, sampleFilepath);
    assert.neq(x, 0, "bsondump should exit with failure when given both an out file and a positional argument");

    x = _runMongoProgram( "bsondump", "-vvvv", sampleFilepath);
    assert.eq(x, 0, "bsondump should exit with success when given verbosity");
    x = _runMongoProgram( "bsondump", "--verbose", sampleFilepath);
    assert.eq(x, 0, "bsondump should exit with success when given verbosity");

    clearRawMongoProgramOutput()
    x = _runMongoProgram( "bsondump", "--quiet", sampleFilepath);
    assert.eq(x, 0, "bsondump should exit with success when given --quiet");
    var results = rawMongoProgramOutput();
    assert.eq(results.search("found"), -1, "only the found docs should be printed when --quiet is used");
    assert.gt(results.search("I am a string"), -1, "found docs should still be printed when --quiet is used");

    clearRawMongoProgramOutput()
    x = _runMongoProgram( "bsondump", "--help");
    assert.eq(x, 0, "bsondump should exit with success when given --help");
    var results = rawMongoProgramOutput();
    assert.gt(results.search("Usage"), -1, "help text should be printed when given --help");

    clearRawMongoProgramOutput()
    x = _runMongoProgram( "bsondump", "--version");
    assert.eq(x, 0, "bsondump should exit with success when given --version");
    var results = rawMongoProgramOutput();
    assert.gt(results.search("version"), -1, "version info should be printed when given --version");

})();
