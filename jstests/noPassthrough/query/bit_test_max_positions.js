/**
 * Tests that internalQueryMaxBitTestIntermediatePositions limits the number of set bits accepted
 * by a $bitTest expression ($bitsAllSet, $bitsAllClear, $bitsAnySet, $bitsAnyClear) when the
 * bitmask is supplied as BinData.
 */
const kLimitExceededCode = 12244901;
const kBitTestOperators = ["$bitsAllSet", "$bitsAllClear", "$bitsAnySet", "$bitsAnyClear"];

const conn = MongoRunner.runMongod();
assert.neq(conn, null, "mongod failed to start up");
const db = conn.getDB(jsTestName());
const coll = db.bit_test_max_positions;
coll.drop();

assert.commandWorked(coll.insert({x: 7}));

// Lower the log threshold so we can exercise the "large bitPosition vector" warning path with a
// small mask, then verify a $bitTest query whose set-bit count exceeds the threshold still
// completes without error.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryBitTestPositionsLogThreshold: 100}),
);

// 16 bytes of 0xFF = 128 set bits, above the log threshold of 100.
const aboveLogThreshold = HexData(0, "ff".repeat(16));
for (const op of kBitTestOperators) {
    assert.doesNotThrow(
        () => coll.find({x: {[op]: aboveLogThreshold}}).itcount(),
        [],
        `expected ${op} with mask above the log threshold to succeed`,
    );
}

// Set a limit of 64 set bits (the minimum allowed value).
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryMaxBitTestIntermediatePositions: 64}),
);

// 8 bytes of 0xFF = exactly 64 set bits. Should succeed for all operators.
const sixtyFourBits = BinData(0, "//////////8=");
for (const op of kBitTestOperators) {
    assert.doesNotThrow(
        () => coll.find({x: {[op]: sixtyFourBits}}).itcount(),
        [],
        `expected ${op} with 64-bit mask to succeed`,
    );
}

// 8 bytes of 0xFF + 0x01 = 65 set bits, exceeds the limit of 64. Should fail.
const sixtyFiveBits = BinData(0, "//////////8B");
for (const op of kBitTestOperators) {
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
