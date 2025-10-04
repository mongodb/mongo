/**
 * Checks that APIStrictError is thrown if unstable fields are used with 'apiStrict: true'.
 *
 * @tags: [
 *   uses_api_parameters,
 *   # 'explain' does not support stepdowns.
 *   does_not_support_stepdowns,
 *   requires_fcv_70
 * ]
 */

const testDb = db.getSiblingDB(jsTestName());
const collName = "api_version_unstable_fields";
assert.commandWorked(testDb[collName].insert({a: 1}));

const unstableFieldsForAggregate = {
    isMapReduceCommand: false,
    $_requestReshardingResumeToken: false,
    explain: true,
    runtimeConstants: {localNow: new Date(), userRoles: [], clusterTime: Timestamp(1, 1)},
    collectionUUID: UUID(),
};

const unstableFieldsForFind = {
    min: {"a": 1},
    max: {"a": 1},
    returnKey: false,
    noCursorTimeout: false,
    showRecordId: false,
    tailable: false,
    oplogReplay: false,
    awaitData: false,
    readOnce: false,
    $_requestResumeToken: false,
    $_resumeAfter: {},
};

// Test that command with unstable fields and 'apiStrict: true' throws.
function testCommandWithUnstableFields(command, containsUnstableFields) {
    for (let field in containsUnstableFields) {
        const cmd = JSON.parse(JSON.stringify(command));
        const cmdWithUnstableField = Object.assign(cmd, {[field]: containsUnstableFields[field]});

        assert.commandFailedWithCode(
            testDb.runCommand(cmdWithUnstableField),
            ErrorCodes.APIStrictError,
            cmdWithUnstableField,
        );
    }
}

const aggCmd = {
    aggregate: collName,
    pipeline: [],
    cursor: {},
    apiVersion: "1",
    apiStrict: true,
};
const findCmd = {
    find: collName,
    apiVersion: "1",
    apiStrict: true,
};

testCommandWithUnstableFields(aggCmd, unstableFieldsForAggregate);
testCommandWithUnstableFields(findCmd, unstableFieldsForFind);

// Test that creating unstable indexes with 'apiStrict: true' throws.
let createIndexesCmd = {
    createIndexes: collName,
    indexes: [{key: {a: "text"}, name: "a_1"}],
    apiVersion: "1",
    apiStrict: true,
};
assert.commandFailedWithCode(testDb.runCommand(createIndexesCmd), ErrorCodes.APIStrictError, createIndexesCmd);

createIndexesCmd["indexes"] = [{key: {a: "geoHaystack"}, name: "a_1"}];
assert.commandFailedWithCode(testDb.runCommand(createIndexesCmd), ErrorCodes.CannotCreateIndex, createIndexesCmd);

// Test that collMod command with an unstable field ('prepareUnique') in an inner struct throws when
// 'apiStrict' is set to true.
assert.commandWorked(testDb.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}));
let collModCommand = {collMod: "col", apiVersion: "1", apiStrict: true};
testCommandWithUnstableFields(collModCommand, {index: {name: "a_1", prepareUnique: true}});
