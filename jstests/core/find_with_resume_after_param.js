/**
 * Tests that the internal parameter "$_resumeAfter" validates the type of the 'recordId' for
 * clustered and non clustered collections.
 * @tags: [
 *   # Queries on mongoS may not request or provide a resume token.
 *   assumes_against_mongod_not_mongos,
 *   cannot_run_during_upgrade_downgrade,
 *   # Tailable cursors on replicated capped clustered collections require majority read concern.
 *   assumes_read_concern_unchanged,
 *   # Reading your own writes with majority read concern requires majority write concern (which is
 *   # the implicit default write concern)
 *   assumes_write_concern_unchanged,
 *   # Does not support multiplanning, because it makes explain fail
 *   does_not_support_multiplanning_single_solutions,
 *   # Transactions do not support writes on capped collections
 *   does_not_support_transactions,
 *   requires_capped,
 * ]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

const sbeFullyEnabled = checkSbeFullyEnabled(db);

const clustered = db.clusteredColl;
const nonClustered = db.normalColl;
const clusteredName = clustered.getName();
const nonClusteredName = nonClustered.getName();

assertDropCollection(db, clusteredName);
assertDropCollection(db, nonClusteredName);

db.createCollection(clusteredName, {clusteredIndex: {key: {_id: 1}, unique: true}});
db.createCollection(nonClusteredName);

// Verify that we get a 'null' resume token when we request a resumeToken from an empty collection.
let emptyRes = assert.commandWorked(db.runCommand({
    find: nonClusteredName,
    filter: {},
    $_requestResumeToken: true,
    hint: {$natural: 1},
    limit: 3,
}));
assert(emptyRes.hasOwnProperty('cursor'), emptyRes);
assert.eq(emptyRes.cursor.postBatchResumeToken.$recordId, null, emptyRes);

emptyRes = assert.commandWorked(db.runCommand({
    find: clusteredName,
    filter: {},
    $_requestResumeToken: true,
    hint: {$natural: 1},
    limit: 3,
}));
assert(emptyRes.hasOwnProperty('cursor'), emptyRes);
assert.eq(emptyRes.cursor.postBatchResumeToken.$recordId, null, emptyRes);

// Insert some documents.
const docs = [{_id: 1, a: 1}, {_id: 2, a: 2}, {_id: 3, a: 3}];
assert.commandWorked(clustered.insertMany(docs));
assert.commandWorked(nonClustered.insertMany(docs));

// Obtain a null RecordId when we hit EOF and request a postBatchResumeToken.
const nullRecord = assert.commandWorked(db.runCommand({
    find: nonClusteredName,
    filter: {},
    $_requestResumeToken: true,
    hint: {$natural: 1},
}));

assert(nullRecord.hasOwnProperty('cursor'), nullRecord);
assert.eq(nullRecord.cursor.postBatchResumeToken.$recordId, null, nullRecord);

// Obtain the RecordId of the last record in the collection. Note that we avoid hitting EOF by
// adding a 'limit: 3' in our query (if we hit EOF, our returned RecordId would be 'null').
const lastRecord = assert.commandWorked(db.runCommand({
    find: nonClusteredName,
    filter: {},
    $_requestResumeToken: true,
    hint: {$natural: 1},
    limit: 3,
}));

assert(lastRecord.hasOwnProperty('cursor'), lastRecord);
assert.neq(lastRecord.cursor.postBatchResumeToken, null, lastRecord);

// When given the recordId of the last record in the collection, we should receive a null
// resumeToken when our cursor is exhausted.
const res = assert.commandWorked(db.runCommand({
    find: nonClusteredName,
    filter: {},
    $_requestResumeToken: true,
    $_resumeAfter: lastRecord.cursor.postBatchResumeToken,
    hint: {$natural: 1}
}));
assert(res.hasOwnProperty('cursor'), res);
const cursor = res['cursor'];
assert(cursor.hasOwnProperty('postBatchResumeToken'), res);

// Note that we don't perform an exact equality on 'postBatchResumeToken' because depending on
// the configuration, it may contain additional fields (such as '$initialSyncId').
assert.eq(cursor.postBatchResumeToken.$recordId, null, res);

// Test '$_requestResumeToken' with tailable cursors against capped collections.
const cappedClusteredName = clusteredName + "_capped";
const cappedNonClusteredName = nonClusteredName + "_capped";

assert.commandWorked(db.createCollection(
    cappedClusteredName,
    {clusteredIndex: {key: {_id: 1}, unique: true}, capped: true, expireAfterSeconds: 2000}));
assert.commandWorked(db.createCollection(cappedNonClusteredName, {capped: true, size: 10 * 1024}));

// Add a document. We should get a non-null RecordId, even if we hit EOF.
assert.commandWorked(db[cappedNonClusteredName].insertMany([{_id: 1}, {_id: 2}]));
assert.commandWorked(db[cappedClusteredName].insertMany([{_id: 1}, {_id: 2}]));

// TODO SERVER-84205 Remove this SBE check once SBE properly supports tailable cursors.
if (!sbeFullyEnabled) {
    let tailableRes = assert.commandWorked(db.runCommand({
        find: cappedNonClusteredName,
        filter: {},
        $_requestResumeToken: true,
        tailable: true,
        hint: {$natural: 1},
    }));

    assert(tailableRes.hasOwnProperty('cursor'), tailableRes);
    assert(tailableRes.cursor.hasOwnProperty('postBatchResumeToken'), tailableRes);
    assert.neq(tailableRes.cursor.postBatchResumeToken, null, tailableRes);

    tailableRes = assert.commandWorked(db.runCommand({
        find: cappedClusteredName,
        filter: {},
        $_requestResumeToken: true,
        tailable: true,
        hint: {$natural: 1},
        readConcern: {level: "majority"},
    }));
    assert(tailableRes.hasOwnProperty('cursor'), tailableRes);
    assert(tailableRes.cursor.hasOwnProperty('postBatchResumeToken'), tailableRes);
    assert.neq(tailableRes.cursor.postBatchResumeToken, null, tailableRes);
}

assert(db[cappedNonClusteredName].drop());
assert(db[cappedClusteredName].drop());

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
