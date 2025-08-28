// This test creates a large projection, which causes a set of field names to
// be stored in a StringMap (based on UnorderedFastKeyTable).  The hash table
// starts with 20 slots, but must be grown repeatedly to hold the complete set
// of fields.  This test verifies that we can grow the hash table repeatedly
// with no failures.
//
// Related to SERVER-9824.
//
// @tags: [
//   operations_longer_than_stepdown_interval_in_txns,
//   requires_getmore,
// ]

let testDB = db.getSiblingDB("grow_hash_table");

let doTest = function (count) {
    print("Testing with count of " + count);
    testDB.dropDatabase();
    let id = {data: 1};
    let doc = {_id: id};
    let projection = {};

    // Create a document and a projection with fields r1, r2, r3 ...
    for (let i = 1; i <= count; ++i) {
        let r = "r" + i;
        doc[r] = i;
        projection[r] = 1;
    }

    // Store the document
    assert.commandWorked(testDB.collection.insert(doc));

    // Try to read the document using a large projection
    try {
        let findCount = testDB.collection.find({_id: id}, projection).itcount();
        assert(findCount == 1, "Failed to find single stored document, find().itcount() == " + findCount);
    } catch (e) {
        testDB.dropDatabase();
        doassert("Test FAILED!  Caught exception " + tojsononeline(e));
    }
    testDB.dropDatabase();
    jsTest.log("Test PASSED");
};

doTest(10000);
