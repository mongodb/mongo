'use_strict';

function runReadOnlyTest(test) {

    assert.eq(typeof(test.exec), "function");
    assert.eq(typeof(test.load), "function");
    assert.eq(typeof(test.name), "string");

    // TODO: read storageEngine from testData when read-only mode is supported in WiredTiger.
    var options = {
        storageEngine: 'mmapv1',
        nopreallocj: ''
    };

    var writableMongod = MongoRunner.runMongod(options);
    var dbpath = writableMongod.dbpath;

    jsTest.log("starting load phase for test: " + test.name);
    test.load(writableMongod.getDB("test")[test.name]);

    MongoRunner.stopMongod(writableMongod);

    // TODO: change directory to read-only permissions when RO is implemented in MMAPv1.
    var readOnlyOptions = Object.extend(options,
                                        {readOnly: '',
                                         dbpath: dbpath,
                                         noCleanData: true});

    var readOnlyMongod = MongoRunner.runMongod(readOnlyOptions);

    jsTest.log("starting execution phase for test: " + test.name);
    test.exec(readOnlyMongod.getDB("test")[test.name]);

    MongoRunner.stopMongod(readOnlyMongod);
}
