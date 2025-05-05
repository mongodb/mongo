// Regression test to check that different document sizes work correctly with $lookup.
(function() {
'use strict';

load('jstests/libs/fixture_helpers.js');  // For 'FixtureHelpers'

const localColl = db.lookup_spill_local;
const foreignColl = db.lookup_spill_foreign;
localColl.drop();
foreignColl.drop();

const memoryLimit = 128;  // Spill at 128 bytes

function setHashLookupMemoryLimit(memoryLimit) {
    const commandResArr = FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: memoryLimit,
        }
    });
    assert.gt(commandResArr.length, 0, "Setting memory limit on primaries failed");
    assert.commandWorked(commandResArr[0]);
}

function runHashLookupSpill() {
    const smallStr = "small";
    const bigStr = Array(memoryLimit).toString();
    const localDoc = {_id: 1, a: 2};
    const foreignDocs = [
        {_id: 0, b: 1, padding: smallStr},
        {_id: 1, b: 2, padding: bigStr},
        {_id: 2, b: 1, padding: smallStr},
        {_id: 3, b: 2, padding: bigStr},
        {_id: 4, b: 1, padding: smallStr},
        {_id: 5, b: 2, padding: bigStr},
        {_id: 6, b: 1, padding: smallStr},
        {_id: 7, b: 2, padding: bigStr},
        {_id: 8, b: 1, padding: smallStr},
    ];

    assert.commandWorked(localColl.insert(localDoc));
    assert.commandWorked(foreignColl.insertMany(foreignDocs));
    const pipeline = [
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "matched"}},
        {$sort: {_id: 1}}
    ];

    const result = localColl.aggregate(pipeline).toArray();
    assert.eq(result.length, 1, result);
    assert.eq(result[0].matched.length, 4, result);
    for (let matched of result[0].matched) {
        assert.eq(matched.padding, bigStr);
    }
}

const oldMemoryLimit =
    assert
        .commandWorked(db.adminCommand({
            getParameter: 1,
            internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1
        }))
        .internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill;

try {
    setHashLookupMemoryLimit(memoryLimit);
    runHashLookupSpill();
} finally {
    setHashLookupMemoryLimit(oldMemoryLimit);
}
})();
