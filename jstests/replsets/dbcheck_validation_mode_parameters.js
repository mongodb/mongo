/**
 * Test the validity of parameters in the dbCheck command.
 *
 * @tags: [
 *   requires_fcv_71
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "dbCheckValidationModeParameters";
const colName = "dbCheckValidationModeParameters-collection";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const db = primary.getDB(dbName);
const col = db[colName];
const nDocs = 1000;

assert.commandWorked(col.insertMany([...Array(nDocs).keys()].map(x => ({a: x})), {ordered: false}));
replSet.awaitReplication();

function testFeatureFlagDisabled() {
    jsTestLog("Testing dbCheck with feature flag disabled.");
    // validateMode field is not allowed if feature flag is disabled.
    assert.commandFailedWithCode(db.runCommand({
        dbCheck: colName,
        validateMode: "dataConsistency",
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandWorked(db.runCommand({
        dbCheck: colName,
    }));
}

function testInvalidParameter() {
    jsTestLog("Testing dbCheck with invalid parameters.");
    // Unsupported enum passed in to validateMode field.
    assert.commandFailedWithCode(db.runCommand({
        dbCheck: colName,
        validateMode: "invalidParam",
    }),
                                 ErrorCodes.BadValue);

    // secondaryIndex field must be specified when validateMode is extraIndexKeysCheck.
    assert.commandFailedWithCode(db.runCommand({
        dbCheck: colName,
        validateMode: "extraIndexKeysCheck",
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        db.runCommand(
            {dbCheck: colName, validateMode: "extraIndexKeysCheck", skipLookupForExtraKeys: true}),
        ErrorCodes.InvalidOptions);

    // Neither secondaryIndex nor skipLookupForExtraKeys fields can be specified when validateMode
    // is dataConsistency or dataConsistencyAndMissingIndexKeysCheck.
    assert.commandFailedWithCode(db.runCommand({
        dbCheck: colName,
        validateMode: "dataConsistency",
        secondaryIndex: "secondaryIndex",
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(db.runCommand({
        dbCheck: colName,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        secondaryIndex: "secondaryIndex",
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(db.runCommand({
        dbCheck: colName,
        validateMode: "dataConsistency",
        skipLookupForExtraKeys: true,
    }),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(db.runCommand({
        dbCheck: colName,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        skipLookupForExtraKeys: true,
    }),
                                 ErrorCodes.InvalidOptions);
}

function testValidParameter() {
    jsTestLog("Testing dbCheck with valid parameters.");
    // dataConsistency is a supported enum for the validateMode field.
    assert.commandWorked(db.runCommand({
        dbCheck: colName,
        validateMode: "dataConsistency",
    }));

    // dataConsistencyAndMissingIndexKeysCheck is a supported enum for the validateMode
    // field.
    assert.commandWorked(db.runCommand({
        dbCheck: colName,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
    }));

    // extraIndexKeysCheck is a supported enum for the validateMode field.
    assert.commandWorked(db.runCommand({
        dbCheck: colName,
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "secondaryIndex",
        skipLookupForExtraKeys: true,
    }));
    // skipLookupForExtraKeys is default to false when validateMode is set.
    assert.commandWorked(db.runCommand({
        dbCheck: colName,
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "secondaryIndex",
    }));
}

const secondaryIndexChecks =
    FeatureFlagUtil.isPresentAndEnabled(primary, "SecondaryIndexChecksInDbCheck");
if (secondaryIndexChecks) {
    testInvalidParameter();
    testValidParameter();
} else {
    testFeatureFlagDisabled();
}

replSet.stopSet();
