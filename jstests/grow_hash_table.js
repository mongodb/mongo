// This test creates a large projection, which causes a set of field names to
// be stored in a StringMap (based on UnorderedFastKeyTable).  The hash table
// starts with 20 slots, but must be grown repeatedly to hold the complete set
// of fields.  This test verifies that we can grow the hash table repeatedly
// with no failures.
//
// Related to SERVER-9824.

var testDB = db.getSiblingDB('grow_hash_table');

var doTest = function(count) {
    print('Testing with count of ' + count);
    testDB.dropDatabase();
    var id = { data: 1 };
    var doc = { _id: id };
    var projection = { };

    // Create a document and a projection with fields r1, r2, r3 ...
    for (var i = 1; i <= count; ++i) {
        var r = 'r' + i;
        doc[r] = i;
        projection[r] = 1;
    }

    // Store the document
    testDB.collection.insert(doc);
    var errorObj = testDB.getLastErrorObj();
    assert(errorObj.err == null,
           'Failed to insert document, getLastErrorObj = ' + tojsononeline(errorObj));

    // Try to read the document using a large projection
    try {
        var findCount = testDB.collection.find({ _id: id }, projection).itcount();
        assert(findCount == 1,
               'Failed to find single stored document, find().itcount() == ' + findCount);
    }
    catch (e) {
        testDB.dropDatabase();
        doassert('Test FAILED!  Caught exception ' + tojsononeline(e));
    }
    testDB.dropDatabase();
    jsTest.log('Test PASSED');
}

doTest(10000);
