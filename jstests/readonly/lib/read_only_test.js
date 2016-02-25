'use_strict';

function makeDirectoryReadOnly(dir) {
    if (_isWindows()) {
        run("attrib", "+r", dir, "/s");
    } else {
        run("chmod", "-R", "a-w", dir);
    }
}

function makeDirectoryWritable(dir) {
    if (_isWindows()) {
        run("attrib", "-r", dir, "/s");
    } else {
        run("chmod", "-R", "a+w", dir);
    }
}

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

    makeDirectoryReadOnly(dbpath);

    try {
        var readOnlyOptions = Object.extend(options,
                                            {readOnly: '',
                                             dbpath: dbpath,
                                             noCleanData: true});

        var readOnlyMongod = MongoRunner.runMongod(readOnlyOptions);

        jsTest.log('starting execution phase for test: ' + test.name);
        test.exec(readOnlyMongod.getDB('test')[test.name]);

        // We need to make the directory writable so that MongoRunner can clean the dbpath.
        makeDirectoryWritable(dbpath);
        MongoRunner.stopMongod(readOnlyMongod);
    } finally {
        // One last time, just in case.
        makeDirectoryWritable(dbpath);
    }
}

function* cycleN(arr, N) {
    for (var i = 0; i < N; ++i) {
        yield arr[i % arr.length];
    }
}

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
}
