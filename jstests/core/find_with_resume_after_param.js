/**
 * Tests that the internal parameter "$_resumeAfter" validates the type of the 'recordId' for
 * clustered and non clustered collections.
 * @tags: [
 *   # Queries on mongoS may not request or provide a resume token.
 *   assumes_against_mongod_not_mongos,
 *   cannot_run_during_upgrade_downgrade,
 *   # Does not support multiplanning, because it makes explain fail
 *   does_not_support_multiplanning_single_solutions,
 * ]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const clustered = db.clusteredColl;
const nonClustered = db.normalColl;
const clusteredName = clustered.getName();
const nonClusteredName = nonClustered.getName();

assertDropCollection(db, clusteredName);
assertDropCollection(db, nonClusteredName);

db.createCollection(clusteredName, {clusteredIndex: {key: {_id: 1}, unique: true}});
db.createCollection(nonClusteredName);

// Insert some documents.
const docs = [{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}];
assert.commandWorked(clustered.insertMany(docs));
assert.commandWorked(nonClustered.insertMany(docs));

// When given the recordId of the last record in the collection, we should receive a null
// resumeToken.
const res = assert.commandWorked(db.runCommand({
    find: nonClusteredName,
    filter: {},
    $_requestResumeToken: true,
    $_resumeAfter: {'$recordId': NumberLong(3)},
    hint: {$natural: 1}
}));
assert(res.hasOwnProperty('cursor'), res);
const cursor = res['cursor'];
assert(cursor.hasOwnProperty('postBatchResumeToken'), res);

// Note that we don't perform an exact equality on 'postBatchResumeToken' because depending on
// the configuration, it may contain additional fields (such as '$initialSyncId').
assert.eq(cursor.postBatchResumeToken.$recordId, null, res);

function validateFailedResumeAfterInFind({collName, resumeAfterSpec, errorCode, explainFail}) {
    const spec = {
        find: collName,
        filter: {},
        $_requestResumeToken: true,
        $_resumeAfter: resumeAfterSpec,
        hint: {$natural: 1}
    };
    assert.commandFailedWithCode(db.runCommand(spec), errorCode);
    // Run the same query under an explain.
    if (explainFail) {
        assert.commandFailedWithCode(db.runCommand({explain: spec}), errorCode);
    } else {
        assert.commandWorked(db.runCommand({explain: spec}));
    }
}

function validateFailedResumeAfterInAggregate({collName, resumeAfterSpec, errorCode, explainFail}) {
    const spec = {
        aggregate: collName,
        pipeline: [],
        $_requestResumeToken: true,
        $_resumeAfter: resumeAfterSpec,
        hint: {$natural: 1},
        cursor: {}
    };
    assert.commandFailedWithCode(db.runCommand(spec), errorCode);
    // Run the same query under an explain.
    if (explainFail) {
        assert.commandFailedWithCode(db.runCommand({explain: spec}), errorCode);
    } else {
        assert.commandWorked(db.runCommand({explain: spec}));
    }
}

function testResumeAfter(validateFunction) {
    //  Confirm $_resumeAfter will fail for clustered collections if the recordId is Long.
    validateFunction({
        collName: clusteredName,
        resumeAfterSpec: {'$recordId': NumberLong(2)},
        errorCode: 7738600,
        explainFail: true
    });

    // Confirm $_resumeAfter will fail with 'KeyNotFound' if given a non existent recordId.
    validateFunction({
        collName: clusteredName,
        resumeAfterSpec: {'$recordId': BinData(5, '1234')},
        errorCode: ErrorCodes.KeyNotFound
    });

    // Confirm $_resumeAfter will fail for normal collections if it is of type BinData.
    validateFunction({
        collName: nonClusteredName,
        resumeAfterSpec: {'$recordId': BinData(5, '1234')},
        errorCode: 7738600,
        explainFail: true
    });

    // Confirm $_resumeAfter token will fail with 'KeyNotFound' if given a non existent recordId.
    validateFunction({
        collName: nonClusteredName,
        resumeAfterSpec: {'$recordId': NumberLong(8)},
        errorCode: ErrorCodes.KeyNotFound
    });

    // Confirm $_resumeAfter token will fail with 'KeyNotFound' if given a null recordId.
    validateFunction({
        collName: nonClusteredName,
        resumeAfterSpec: {'$recordId': null},
        errorCode: ErrorCodes.KeyNotFound
    });

    // Confirm $_resumeAfter will fail to parse if collection does not exist.
    validateFunction({
        collName: "random",
        resumeAfterSpec: {'$recordId': null, "anotherField": null},
        errorCode: ErrorCodes.BadValue,
        explainFail: true
    });
    validateFunction({
        collName: "random",
        resumeAfterSpec: "string",
        errorCode: ErrorCodes.TypeMismatch,
        explainFail: true
    });
}

testResumeAfter(validateFailedResumeAfterInFind);
// TODO(SERVER-77873): remove "featureFlagReshardingImprovements"
if (FeatureFlagUtil.isPresentAndEnabled(db, "ReshardingImprovements")) {
    testResumeAfter(validateFailedResumeAfterInAggregate);
}
