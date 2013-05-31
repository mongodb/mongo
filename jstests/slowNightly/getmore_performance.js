// This test creates a collection and tests the performance of a getmore loop reading it.
//
// Related to SERVER-9721.
//

var makeData = function(testDB) {
    var howMany = 2000;
    var arr = [];
    var i;
    for (i = 0; i < 10000; ++i) {
        arr.push(i);
    }
    for (i = 0; i < howMany; ++i) {
        testDB.test.insert({arr: arr});
    }
}

var time = function (f, testDB) {
    var start = new ISODate();
    f(testDB);
    var end = new ISODate();
    return (end - start);
}

var func = function(testDB) {
    var c = testDB.test.find({}, {_id: 1});
    while (c.hasNext()) {
        var d = c.next();
    }
}

var doTest = function() {
    // Create the test collection
    //
    var baseName = 'getmore_performance';
    var dbpath = '/data/db/sconsTests/' + baseName
    var port = allocatePorts( 1 )[ 0 ];
    var mongod = startMongod( '--port', port, '--dbpath', dbpath );
    sleep(50);
    var testDB = new Mongo('localhost:' + port).getDB(baseName);
    testDB.dropDatabase();
    makeData(testDB);
    stopMongod(port, 15);

    // Start a new mongod and read the test collection twice
    //
    mongod = startMongodNoReset( '--port', port, '--dbpath', dbpath );
    sleep(50);
    testDB = new Mongo('localhost:' + port).getDB(baseName);
    var firstPass = time(func, testDB);
    var secondPass = time(func, testDB);
    stopMongod(port, 15);

    // Fail the test if the first pass took considerably longer than the second pass
    //
    var factor = 10;
    var maxAcceptableMilliseconds = secondPass * factor;
    jsTest.log('First pass took ' + firstPass +
               ' ms, second pass took ' + secondPass +
               ' ms, setting cutoff at ' + maxAcceptableMilliseconds +
               ' ms (factor of ' + factor + ')');
    assert.lte(firstPass, maxAcceptableMilliseconds, 'Performance test FAILED!');
    jsTest.log('Performance test PASSED!');
}

doTest();
