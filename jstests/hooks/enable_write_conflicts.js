(function() {
'use strict';

const chance = TestData.WTWriteConflictExceptionChance;
assert.gte(chance, 0, "WTWriteConflictExceptionChance must be >= 0");
assert.lte(chance, 1, "WTWriteConflictExceptionChance must be <= 1");

const readChance = TestData.WTWriteConflictExceptionForReadsChance;
assert.gte(readChance, 0, "WTWriteConflictExceptionForReadsChance must be >= 0");
assert.lte(readChance, 1, "WTWriteConflictExceptionForReadsChance must be <= 1");

assert.commandWorked(db.adminCommand(
    {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: chance}}));

assert.commandWorked(db.adminCommand({
    configureFailPoint: 'WTWriteConflictExceptionForReads',
    mode: {activationProbability: readChance}
}));
})();
