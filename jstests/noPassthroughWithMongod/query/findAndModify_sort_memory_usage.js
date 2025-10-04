// SERVER-16469: test that the findAndModify command will correctly sort a large amount of data
// without hitting the internal sort memory limit (using a "top-K sort", where K=1).
//
// Note that this test sets the server parameter "internalQueryMaxBlockingSortMemoryUsageBytes", and
// restores the original value of the parameter before exiting.  As a result, this test cannot run
// in the sharding passthrough (because mongos does not have this parameter), and cannot run in the
// parallel suite (because the change of the parameter value would interfere with other tests).

let coll = db[jsTestName()];
coll.drop();

// Set the internal sort memory limit to 1MB.
let result = db.adminCommand({getParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1});
assert.commandWorked(result);
let oldSortLimit = result.internalQueryMaxBlockingSortMemoryUsageBytes;
let newSortLimit = 1024 * 1024;
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: newSortLimit}));
assert.commandWorked(db.adminCommand({setParameter: 1, allowDiskUseByDefault: false}));

try {
    // Insert ~3MB of data.
    let largeStr = "";
    for (var i = 0; i < 32 * 1024; ++i) {
        largeStr += "x";
    }
    for (var i = 0; i < 100; ++i) {
        assert.commandWorked(coll.insert({a: largeStr, b: i}));
    }

    // Verify that an unindexed sort of this data fails with a find() if no limit is specified.
    assert.throws(function () {
        coll.find({}).sort({b: 1}).itcount();
    });

    // Verify that an unindexed sort of this data succeeds with findAndModify (which should be
    // requesting a top-K sort).
    result = coll.runCommand({findAndModify: coll.getName(), query: {}, update: {$set: {c: 1}}, sort: {b: 1}});
    assert.commandWorked(result);
    assert.neq(result.value, null);
    assert.eq(result.value.b, 0);
} finally {
    // Restore the orginal sort memory limit.
    assert.commandWorked(db.adminCommand({setParameter: 1, allowDiskUseByDefault: true}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: oldSortLimit}),
    );
}
