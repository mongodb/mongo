// The test runs commands that are not allowed with security token: getLog.
// @tags: [
//   not_allowed_with_security_token,
//   does_not_support_stepdowns,
//   requires_capped,
// ]

// Check that a warning message about doing a capped collection scan for a query with an _id
// constraint is printed at appropriate times.  SERVER-5353

function numWarnings() {
    let logs = db.adminCommand({getLog: "global"}).log;
    let ret = 0;
    logs.forEach(function(x) {
        if (x.match(warningMatchRegexp)) {
            ++ret;
        }
    });
    return ret;
}

let collectionNameIndex = 0;

// Generate a collection name not already present in the log.
do {
    var testCollectionName = 'jstests_queryoptimizera__' + collectionNameIndex++;
    var warningMatchString =
        'unindexed _id query on capped collection.*collection: test.' + testCollectionName;
    var warningMatchRegexp = new RegExp(warningMatchString);

} while (numWarnings() > 0);

let t = db[testCollectionName];
t.drop();

let notCappedCollectionName = testCollectionName + '_notCapped';

let notCapped = db.getSiblingDB("local").getCollection(notCappedCollectionName);
notCapped.drop();

assert.commandWorked(db.createCollection(testCollectionName, {capped: true, size: 1000}));
assert.commandWorked(
    notCapped.getDB().createCollection(notCappedCollectionName, {autoIndexId: false}));

t.insert({});
notCapped.insert({});

let oldNumWarnings = 0;

function assertNoNewWarnings() {
    assert.eq(oldNumWarnings, numWarnings());
}

function assertNewWarning() {
    let newNumWarnings = numWarnings();
    // Ensure that newNumWarnings > oldNumWarnings.  It's not safe to test that oldNumWarnings + 1
    // == newNumWarnings, because a (simulated) page fault exception may cause multiple messages to
    // be logged instead of only one.
    assert.lt(oldNumWarnings, newNumWarnings);
    oldNumWarnings = newNumWarnings;
}

// Simple _id query
t.find({_id: 0}).itcount();
assertNoNewWarnings();

// Simple _id query without an _id index, on a non capped collection.
notCapped.find({_id: 0}).itcount();
assertNoNewWarnings();

// A multi field query, including _id.
t.find({_id: 0, a: 0}).itcount();
assertNoNewWarnings();

// An unsatisfiable query.
t.find({_id: 0, a: {$in: []}}).itcount();
assertNoNewWarnings();

// An hinted query.
t.find({_id: 0}).hint({$natural: 1}).itcount();
assertNoNewWarnings();

// Retry a multi field query.
t.find({_id: 0, a: 0}).itcount();
assertNoNewWarnings();

// Warnings should not be printed when an index is added on _id.
t.createIndex({_id: 1});

t.find({_id: 0}).itcount();
assertNoNewWarnings();

t.find({_id: 0, a: 0}).itcount();
assertNoNewWarnings();

t.find({_id: 0, a: 0}).itcount();
assertNoNewWarnings();

t.drop();  // cleanup
notCapped.drop();
