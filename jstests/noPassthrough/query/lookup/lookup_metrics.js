/**
 * Tests that the lookup metrics are recorded correctly in serverStatus.
 *
 * @tags: [featureFlagSbeFull]
 */

const conn = MongoRunner.runMongod({setParameter: {allowDiskUseByDefault: true}});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());
assert.commandWorked(db.dropDatabase());

// Set up the database.
assert.commandWorked(db.students.insertMany([
    {sID: 22001, name: "Alex", year: 1, score: 4.0},
    {sID: 21001, name: "Bernie", year: 2, score: 3.7},
    {sID: 20010, name: "Chris", year: 3, score: 2.5},
    {sID: 22021, name: "Drew", year: 1, score: 3.2},
    {sID: 17301, name: "Harley", year: 6, score: 3.1},
    {sID: 21022, name: "Farmer", year: 1, score: 2.2},
    {sID: 20020, name: "George", year: 3, score: 2.8},
    {sID: 18020, name: "Harley", year: 5, score: 2.8},
]));

assert.commandWorked(db.createView("firstYears", "students", [{$match: {year: 1}}]));

assert.commandWorked(db.people.insertMany([
    {pID: 1000, name: "Alex"},
    {pID: 1001, name: "Drew"},
    {pID: 1002, name: "Justin"},
    {pID: 1003, name: "Parker"},
]));

const lookupStrategy = {
    nestedLoopJoin: "nestedLoopJoin",
    indexedLoopJoin: "indexedLoopJoin",
    hashLookup: "hashLookup",
    dynamicIndexedLoopJoin: "dynamicIndexedLoopJoin",
    nonSbe: "nonSbe"
};

// Create an object with the correct lookup counter values after the specified type of query.
function generateExpectedCounters(
    joinStrategy = lookupStrategy.nonSbe, spillToDisk = 0, spillToDiskBytes = 0) {
    let counters = db.serverStatus().metrics.query.lookup;
    assert(counters, "counters did not exist");
    let expected = Object.assign(counters);
    switch (joinStrategy) {
        case lookupStrategy.nestedLoopJoin:
            expected.nestedLoopJoin = NumberLong(expected.nestedLoopJoin + 1);
            break;
        case lookupStrategy.indexedLoopJoin:
            expected.indexedLoopJoin = NumberLong(expected.indexedLoopJoin + 1);
            break;
        case lookupStrategy.dynamicIndexedLoopJoin:
            expected.dynamicIndexedLoopJoin = NumberLong(expected.dynamicIndexedLoopJoin + 1);
            break;
        case lookupStrategy.hashLookup: {
            expected.hashLookup = NumberLong(expected.hashLookup + 1);

            const spills = NumberLong(expected.hashLookupSpills + spillToDisk);
            expected.hashLookupSpillToDisk = spills;  // legacy metric
            expected.hashLookupSpills = spills;       // new metric
            // every spill, spills a record unless it is an update when the records do not increase.
            expected.hashLookupSpilledRecords = spills;

            const spilledBytes = NumberLong(expected.hashLookupSpilledBytes + spillToDiskBytes);
            expected.hashLookupSpillToDiskBytes = spilledBytes;  // legacy metric
            expected.hashLookupSpilledBytes = spilledBytes;      // new metric;
        } break;
    }
    return expected;
}

// Compare the values of the lookup counters to an object that represents the expected values.
function compareLookupCounters(expectedCounters) {
    let counters = db.serverStatus().metrics.query.lookup;

    // We cannot compute the spilled data storage size accurately, so we just set it the actual
    // value to make comparison between expected and actual counters more straightforward.
    expectedCounters.hashLookupSpilledDataStorageSize = counters.hashLookupSpilledDataStorageSize;
    // The actual value can be 1 less than the expected if the spill did not add a new record but
    // updated an existing one.
    assert.between(expectedCounters.hashLookupSpilledRecords - 1,
                   counters.hashLookupSpilledRecords,
                   expectedCounters.hashLookupSpilledRecords);
    expectedCounters.hashLookupSpilledRecords = counters.hashLookupSpilledRecords;

    assert.docEq(expectedCounters, counters);
}

// Run a lookup pipeline that does not get pushed down to SBE because it's querying against a view.
let expectedCounters = generateExpectedCounters();
assert.eq(
    db.people
        .aggregate([
            {$lookup: {from: "firstYears", localField: "name", foreignField: "name", as: "matches"}}
        ])
        .itcount(),
    4 /* Matching results */);
compareLookupCounters(expectedCounters);

const pipeline =
    [{$lookup: {from: "students", localField: "name", foreignField: "name", as: "matches"}}];

// Run a lookup pipeline with a hash lookup that gets pushed down to SBE.
expectedCounters = generateExpectedCounters(lookupStrategy.hashLookup);
assert.eq(db.people.aggregate(pipeline).itcount(), 4 /* Matching results */);
compareLookupCounters(expectedCounters);

// Run a lookup pipeline without disk use so that it will use NLJ.
expectedCounters = generateExpectedCounters(lookupStrategy.nestedLoopJoin);
assert.eq(db.people.aggregate(pipeline, {allowDiskUse: false}).itcount(), 4 /* Matching results */);
compareLookupCounters(expectedCounters);

// Create an index for the foreign collection so the query uses INLJ.
assert.commandWorked(db["students"].createIndex({name: 1}));
expectedCounters = generateExpectedCounters(lookupStrategy.indexedLoopJoin);
assert.eq(db.people.aggregate(pipeline).itcount(), 4 /* Matching results */);
compareLookupCounters(expectedCounters);

// Change the collation of the query to be incompatible with the collation of the foreign
// collection.
expectedCounters = generateExpectedCounters(lookupStrategy.dynamicIndexedLoopJoin);
assert.eq(
    db.people
        .aggregate(
            [{
                $lookup: {from: "students", localField: "name", foreignField: "name", as: "matches"}
            }],
            {collation: {locale: "fr"}, allowDiskUse: false})
        .itcount(),
    4 /* Matching results */);
compareLookupCounters(expectedCounters);

assert.commandWorked(db["students"].dropIndexes());

// Reduce the threshold for spilling with hash lookup and then run a query that will spill.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1,
}));
expectedCounters = generateExpectedCounters(lookupStrategy.hashLookup,
                                            16 /* 2 spills per foreign collection row */,
                                            1130 /* spillToDiskBytes */);
assert.eq(db.people.aggregate(pipeline).itcount(), 4 /* Matching results */);
compareLookupCounters(expectedCounters);

MongoRunner.stopMongod(conn);
