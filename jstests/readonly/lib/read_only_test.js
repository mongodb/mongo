'use_strict';

function runReadOnlyTest(test) {

    printjson(test);

    assert.eq(typeof(test.exec), 'function');
    assert.eq(typeof(test.load), 'function');
    assert.eq(typeof(test.name), 'string');

    // TODO: read storageEngine from testData when read-only mode is supported in WiredTiger.
    var options = {
        storageEngine: 'mmapv1',
        nopreallocj: ''
    };

    var writableMongod = MongoRunner.runMongod(options);
    var dbpath = writableMongod.dbpath;

    jsTest.log('starting load phase for test: ' + test.name);
    test.load(writableMongod.getDB('test')[test.name]);

    MongoRunner.stopMongod(writableMongod);

    // TODO: change directory to read-only permissions when RO is implemented in MMAPv1.
    var readOnlyOptions = Object.extend(options,
                                        {readOnly: '',
                                         dbpath: dbpath,
                                         noCleanData: true});

    var readOnlyMongod = MongoRunner.runMongod(readOnlyOptions);

    jsTest.log('starting execution phase for test: ' + test.name);
    test.exec(readOnlyMongod.getDB('test')[test.name]);

    MongoRunner.stopMongod(readOnlyMongod);
}

function* cycleN(arr, N) {
    for (var i = 0; i < N; ++i) {
        yield arr[i % arr.length];
    }
};

function* zip2(iter1, iter2)  {
    var n1 = iter1.next();
    var n2 = iter2.next();
    while (!n1.done || !n2.done) {
        var res = [];
        if (!n1.done) {
            res.push(n1.value);
            n1 = iter1.next();
        }
        if (!n2.done) {
            res.push(n2.value);
            n2 = iter2.next();
        }

        yield res;
    }
};
