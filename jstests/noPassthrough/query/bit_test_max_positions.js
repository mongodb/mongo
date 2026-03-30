/**
 * Tests that internalQueryMaxBitTestIntermediatePositions limits the number of set bits accepted
 * by a $bitTest expression ($bitsAllSet, $bitsAllClear, $bitsAnySet, $bitsAnyClear) when the
 * bitmask is supplied as BinData.
 */
const kLimitExceededCode = 12244901;

const conn = MongoRunner.runMongod();
assert.neq(conn, null, "mongod failed to start up");
const db = conn.getDB(jsTestName());
const coll = db.bit_test_max_positions;
coll.drop();

assert.commandWorked(coll.insert({x: 7}));

// First, create a large BinData of all 0xFF bytes (well above the log threshold) and verify that a
// $bitTest query with it completes without error. Each '/' in base64 is 0b111111; four of them
// decode to three 0xFF bytes.
const largeAllOnes = BinData(0, "/".repeat(1000000));
for (const op of ["$bitsAllSet", "$bitsAllClear", "$bitsAnySet", "$bitsAnyClear"]) {
    assert.doesNotThrow(
        () => coll.find({x: {[op]: largeAllOnes}}).itcount(),
        [],
        `expected ${op} with large all-ones mask to succeed`,
    );
}

// Set a limit of 64 set bits (the minimum allowed value).
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryMaxBitTestIntermediatePositions: 64}));

// 8 bytes of 0xFF = exactly 64 set bits. Should succeed for all operators.
const sixtyFourBits = BinData(0, "//////////8=");
for (const op of ["$bitsAllSet", "$bitsAllClear", "$bitsAnySet", "$bitsAnyClear"]) {
    assert.doesNotThrow(
        () => coll.find({x: {[op]: sixtyFourBits}}).itcount(),
        [],
        `expected ${op} with 64-bit mask to succeed`,
    );
}

// 8 bytes of 0xFF + 0x01 = 65 set bits, exceeds the limit of 64. Should fail.
const sixtyFiveBits = BinData(0, "//////////8B");
for (const op of ["$bitsAllSet", "$bitsAllClear", "$bitsAnySet", "$bitsAnyClear"]) {
    assert.commandFailedWithCode(
        db.runCommand({find: coll.getName(), filter: {x: {[op]: sixtyFiveBits}}}),
        kLimitExceededCode,
        `expected ${op} with 65-bit mask to fail`,
    );
}

// Values below 64 are invalid since a 64-bit integer can have up to 64 set bits.
assert.commandFailedWithCode(
    db.adminCommand({setParameter: 1, internalQueryMaxBitTestIntermediatePositions: 63}),
    ErrorCodes.BadValue,
    "expected setting internalQueryMaxBitTestIntermediatePositions below 64 to fail",
);
assert.commandFailedWithCode(
    db.adminCommand({setParameter: 1, internalQueryMaxBitTestIntermediatePositions: 0}),
    ErrorCodes.BadValue,
    "expected setting internalQueryMaxBitTestIntermediatePositions to 0 to fail",
);

MongoRunner.stopMongod(conn);
