// Test that serverStatus() features dependent on the ProcessInfo::blockCheckSupported() routine
// work correctly.  These features are db.serverStatus({workingSet:1}).workingSet and
// db.serverStatus().indexCounters.
// Related to SERVER-9242, SERVER-6450.

// Check that an object contains a specific set of fields and only those fields
// NOTE: destroys 'item'
//
var testExpectedFields = function(itemString, item, fieldList) {
    print('Testing ' + itemString + ' for expected fields');
    for (var i = 0; i < fieldList.length; ++i) {
        var field = fieldList[i];
        if (typeof item[field] == 'undefined') {
            doassert('Test FAILED: missing "' + field + '" field');
        }
        delete item[field];
    }
    if (!friendlyEqual({}, item)) {
        doassert('Test FAILED: found unexpected field(s): ' + tojsononeline(item));
    }
}

// Run test as function to keep cruft out of global namespace
//
var doTest = function () {

    print('Testing workingSet and indexCounters portions of serverStatus');
    var hostInfo = db.hostInfo();
    var isXP = (hostInfo.os.name == 'Windows XP') ? true : false;
    var isEmpty = (hostInfo.os.name == '') ? true : false;

    // Check that the serverStatus command returns something for these sub-documents
    //
    var serverStatus = db.serverStatus({ workingSet: 1 });
    if (!serverStatus) {
        doassert('Test FAILED: db.serverStatus({workingSet:1}) did not return a value');
    }
    if (!serverStatus.workingSet) {
        doassert('Test FAILED: db.serverStatus({workingSet:1}).workingSet was not returned');
    }
    if (!serverStatus.indexCounters) {
        doassert('Test FAILED: db.serverStatus().indexCounters was not returned');
    }
    var workingSet_1 = serverStatus.workingSet;
    var indexCounters_1 = serverStatus.indexCounters;

    if (isXP) {
        // Windows XP is the only supported platform that should be missing this data; make sure
        // that we don't get bogus data back
        //
        var expectedResult = { info: 'not supported' };
        print('Testing db.serverStatus({workingSet:1}).workingSet on Windows XP -- expecting ' +
              tojsononeline(expectedResult));
        assert.eq(expectedResult, workingSet_1,
                  'Test FAILED: db.serverStatus({workingSet:1}).workingSet' +
                  ' did not return the expected value');
        expectedResult = { note: 'not supported on this platform' };
        print('Testing db.serverStatus().indexCounters on Windows XP -- expecting ' +
              tojsononeline(expectedResult));
        assert.eq(expectedResult, indexCounters_1,
                  'Test FAILED: db.serverStatus().indexCounters' +
                  ' did not return the expected value');
    }
    else if (isEmpty) {
        // Until SERVER-9325 is fixed, Solaris/SmartOS will also be missing this data; make sure
        // that we don't get bogus data back
        //
        expectedResult = { info: 'not supported' };
        print('Testing db.serverStatus({workingSet:1}).workingSet on "" (Solaris?) -- expecting ' +
              tojsononeline(expectedResult));
        assert.eq(expectedResult, workingSet_1,
                  'Test FAILED: db.serverStatus({workingSet:1}).workingSet' +
                  ' did not return the expected value');
        expectedResult = { note: 'not supported on this platform' };
        print('Testing db.serverStatus().indexCounters on "" (Solaris?) -- expecting ' +
              tojsononeline(expectedResult));
        assert.eq(expectedResult, indexCounters_1,
                  'Test FAILED: db.serverStatus().indexCounters' +
                  ' did not return the expected value');
    }
    else {
        // Check that we get both workingSet and indexCounters and that all expected
        // fields are present with no unexpected fields
        //
        testExpectedFields('db.serverStatus({workingSet:1}).workingSet',
                           workingSet_1,
                           ['note', 'pagesInMemory', 'computationTimeMicros', 'overSeconds']);
        testExpectedFields('db.serverStatus().indexCounters',
                           indexCounters_1,
                           ['accesses', 'hits', 'misses', 'resets', 'missRatio']);

        if (0) { // comment out until SERVER-9284 is fixed
        // See if we can make the index counters values change
        //
        print('Testing that indexCounters accesses and hits increase by 1 on indexed find()');
        var blockDB = db.getSiblingDB('block_check_supported');
        blockDB.dropDatabase();
        blockDB.coll.insert({ a: 1 });
        blockDB.coll.ensureIndex({ a: 1 });
        indexCounters_1 = db.serverStatus().indexCounters;
        var doc = blockDB.coll.findOne({ a: 1 });
        var indexCounters_2 = db.serverStatus().indexCounters;
        assert.gt(indexCounters_2.accesses, indexCounters_1.accesses,
                  'Test FAILED: db.serverStatus().indexCounters.accesses' +
                  ' should have had a value greater than ' + indexCounters_1.accesses +
                  ': indexCounters: before find(): ' + tojsononeline(indexCounters_1) +
                  ', after find(): ' + tojsononeline(indexCounters_2));
        assert.gt(indexCounters_2.hits, indexCounters_1.hits,
                  'Test FAILED: db.serverStatus().indexCounters.hits' +
                  ' should have had a value greater than ' + indexCounters_1.hits +
                  ': indexCounters: before find(): ' + tojsononeline(indexCounters_1) +
                  ', after find(): ' + tojsononeline(indexCounters_2));
        } // comment out until SERVER-9284 is fixed
    }
    print('Test PASSED!');
};

doTest();
