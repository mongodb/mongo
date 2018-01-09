(function() {
    "use strict";
    const md = MongoRunner.runMongod({port: "27017", dbpath: MongoRunner.dataPath});
    assert.neq(null, md, "unable to start mongod");
    const targetURI = 'mongodb+srv://test1.test.build.10gen.cc./?ssl=false';
    const exitCode = runMongoProgram('mongo', targetURI, '--eval', ';');
    assert.eq(exitCode, 0, "Failed to connect with a `mongodb+srv://` style URI.");
    MongoRunner.stopMongod(md);
})();
