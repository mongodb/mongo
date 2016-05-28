// Test NamespaceDetails::cappedTruncateAfter with empty extents

Random.setRandomSeed();

t = db.jstests_capped8;

function debug(x) {
    //    printjson( x );
}

/** Generate an object with a string field of specified length */
function obj(size, x) {
    return {X: x, a: new Array(size + 1).toString()};
}

function withinTwo(a, b) {
    assert(Math.abs(a - b) <= 2, "not within one: " + a + ", " + b);
}

var X = 0;

/**
 * Insert enough documents of the given size spec that the collection will
 * contain only documents having this size spec.
 */
function insertManyRollingOver(objsize) {
    // Add some variability, as the precise number can trigger different cases.
    X++;
    n = 250 + Random.randInt(10);

    assert(t.count() == 0 || t.findOne().X != X);

    for (i = 0; i < n; ++i) {
        t.save(obj(objsize, X));
        debug(t.count());
    }

    if (t.findOne().X != X) {
        printjson(t.findOne());
        print("\n\nERROR didn't roll over in insertManyRollingOver " + objsize);
        print("approx amountwritten: " + (objsize * n));
        printjson(t.stats());
        assert(false);
    }
}

/**
 * Insert some documents in such a way that there may be an empty extent, then
 * truncate the capped collection.
 */
function insertAndTruncate(first) {
    myInitialCount = t.count();
    // Insert enough documents to make the capped allocation loop over.
    insertManyRollingOver(150);
    myFiftyCount = t.count();
    // Insert documents that are too big to fit in the smaller extents.
    insertManyRollingOver(3000);
    myTwokCount = t.count();
    if (first) {
        initialCount = myInitialCount;
        fiftyCount = myFiftyCount;
        twokCount = myTwokCount;
        // Sanity checks for collection count
        assert(fiftyCount > initialCount);
        assert(fiftyCount > twokCount);
    } else {
        // Check that we are able to insert roughly the same number of documents
        // after truncating.  The exact values are slightly variable as a result
        // of the capped allocation algorithm and where the remaining entry is.
        withinTwo(initialCount, myInitialCount);
        withinTwo(fiftyCount, myFiftyCount);
        withinTwo(twokCount, myTwokCount);
    }
    count = t.count();
    // Check that we can truncate the collection successfully.
    assert.commandWorked(db.runCommand({captrunc: "jstests_capped8", n: count - 1, inc: false}));
    assert.eq(1, t.count());
}

/** Test truncating and subsequent inserts */
function testTruncate() {
    insertAndTruncate(true);
    insertAndTruncate(false);
    insertAndTruncate(false);
}

var pass = 1;

print("pass " + pass++);
t.drop();
db._dbCommand({create: "jstests_capped8", capped: true, $nExtents: [10000, 10000, 4000]});
testTruncate();

print("pass " + pass++);
t.drop();
db._dbCommand({create: "jstests_capped8", capped: true, $nExtents: [10000, 1000, 4000]});
testTruncate();

print("pass " + pass++);
t.drop();
db._dbCommand({create: "jstests_capped8", capped: true, $nExtents: [10000, 4000]});
testTruncate();

print("pass " + pass++);
t.drop();
db._dbCommand({create: "jstests_capped8", capped: true, $nExtents: [10000]});
testTruncate();

t.drop();
