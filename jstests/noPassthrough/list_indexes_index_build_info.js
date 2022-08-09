/**
 * Tests the listIndexes command's includeIndexBuildInfo flag. When the flag is set, all index specs
 * should be contained within the 'spec' subdocument rather than in the document itself, and for
 * indexes which are building, the indexBuildInfo document should be returned next to spec, and
 * should contain a buildUUID.
 *
 * @tags: [
 *     # Persistent storage engine needed for resumable index builds.
 *     requires_persistence,
 *     requires_replication,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/noPassthrough/libs/index_build.js");  // for IndexBuildTest

/**
 * Given two listIndexes command results, the first without index build info included and the second
 * with, ensures that ready and in-progress indexes are formatted correctly in each output.
 */
function assertListIndexesOutputsMatch(
    withoutBuildInfo, withBuildInfo, readyIndexNames, buildingIndexNames, expectedBuildingInfo) {
    const allIndexNames = readyIndexNames.concat(buildingIndexNames);
    assert.eq(
        withoutBuildInfo.map(i => i.name).sort(),
        allIndexNames.sort(),
        "Expected indexes do not match returned indexes in withoutBuildInfo: Expected names: " +
            tojson(allIndexNames) + ", withoutBuildInfo: " + tojson(withoutBuildInfo));
    assert.eq(withBuildInfo.map(i => i.spec.name).sort(),
              allIndexNames.sort(),
              "Expected indexes do not match returned indexes in withBuildInfo: Expected names: " +
                  tojson(allIndexNames) + ", withBuildInfo: " + tojson(withBuildInfo));

    for (let i = 0; i < withBuildInfo.length; i++) {
        assert.eq(
            withoutBuildInfo[i],
            withBuildInfo[i].spec,
            "Expected withBuildInfo spec to contain the same information as withoutBuildInfo: withoutBuildInfo result: " +
                tojson(withoutBuildInfo[i]) +
                ", withBuildInfo result: " + tojson(withBuildInfo[i]));

        if (readyIndexNames.includes(withBuildInfo[i].spec.name)) {
            // Index is done, no indexBuildInfo
            assert(!withBuildInfo[i].hasOwnProperty('indexBuildInfo'),
                   "Index expected to be done building had indexBuildInfo: " +
                       tojson(withBuildInfo[i]));
        } else {
            // Index building, should have indexBuildInfo.buildUUID
            assert(withBuildInfo[i].hasOwnProperty('indexBuildInfo'),
                   "Index expected to be in-progress building did not have indexBuildInfo: " +
                       tojson(withBuildInfo[i]));
            assert(
                withBuildInfo[i].indexBuildInfo.hasOwnProperty('buildUUID'),
                "Index expected to be in-progress building did not have indexBuildInfo.buildUUID: " +
                    tojson(withBuildInfo[i]));

            // Index building, should have indexBuildInfo.method.
            assert(
                withBuildInfo[i].indexBuildInfo.hasOwnProperty('method'),
                "Index expected to be in-progress building did not have indexBuildInfo.method: " +
                    tojson(withBuildInfo[i]));
            assert.eq(withBuildInfo[i].indexBuildInfo.method,
                      'Hybrid',
                      "Index expected to be in-progress is building with an unexpected method: " +
                          tojson(withBuildInfo[i]));

            // Index building, should have indexBuildInfo.phase.
            assert(withBuildInfo[i].indexBuildInfo.hasOwnProperty('phase'),
                   "Index expected to be in-progress building did not have indexBuildInfo.phase: " +
                       tojson(withBuildInfo[i]));
            assert.eq(withBuildInfo[i].indexBuildInfo.phase,
                      1,
                      "Index expected to be in-progress is building an unexpected phase: " +
                          tojson(withBuildInfo[i]));

            // Index building, should have indexBuildInfo.phaseStr.
            assert(withBuildInfo[i].indexBuildInfo.hasOwnProperty('phaseStr'),
                   "Index expected to be in-progress building did not have indexBuildInfo.Str: " +
                       tojson(withBuildInfo[i]));
            assert.eq(withBuildInfo[i].indexBuildInfo.phaseStr,
                      'collection scan',
                      "Index expected to be in-progress building unexpected phaseStr: " +
                          tojson(withBuildInfo[i]));

            // Index building, should have indexBuildInfo.opid.
            assert(withBuildInfo[i].indexBuildInfo.hasOwnProperty('opid'),
                   "Index expected to be in-progress building did not have indexBuildInfo.opid: " +
                       tojson(withBuildInfo[i]));
            assert.eq(
                withBuildInfo[i].indexBuildInfo.opid,
                expectedBuildingInfo[withBuildInfo[i].spec.name].opid,
                "Index expected to be in-progress building has different opid from db.currentOp(): " +
                    tojson(withBuildInfo[i]) + '; db.currentOp(): ' + tojson(db.currentOp()));

            // Index building, should have indexBuildInfo.resumable.
            assert(
                withBuildInfo[i].indexBuildInfo.hasOwnProperty('resumable'),
                "Index expected to be in-progress building did not have indexBuildInfo.resumable: " +
                    tojson(withBuildInfo[i]));
            assert.eq(withBuildInfo[i].indexBuildInfo.resumable,
                      expectedBuildingInfo[withBuildInfo[i].spec.name].resumable,
                      "Index expected to be in-progress building has unexpected resumable value: " +
                          tojson(withBuildInfo[i]));

            // Index building, should have indexBuildInfo.replicationState.
            assert(
                withBuildInfo[i].indexBuildInfo.hasOwnProperty('replicationState'),
                "Index expected to be in-progress building did not have indexBuildInfo.replicationState: " +
                    tojson(withBuildInfo[i]));
            assert.eq(
                withBuildInfo[i].indexBuildInfo.replicationState.state,
                'In progress',
                "Index expected to be in-progress building has unexpected replication state: " +
                    tojson(withBuildInfo[i]));
        }
    }
}

const rst = ReplSetTest({nodes: [{}, {rsConfig: {votes: 0, priority: 0}}]});
rst.startSet();
rst.initiate();

const conn = rst.getPrimary();
const db = conn.getDB("test");
const collName = "test_list_indexes_index_build_info";
const coll = db.getCollection(collName);
coll.drop();
db.createCollection(collName);

// Add data to the collection so that we don't hit createIndexOnEmptyCollection. This is important
// so that we actually hit the failpoint which is set by IndexBuildTest.pauseIndexBuilds.
coll.insert({a: 600, b: 700});

// Create a new index.
const doneIndexName = "a_1";
assert.commandWorked(
    db.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: doneIndexName}]}));

// Ensure that the format of the listIndexes output still changes in the index build complete case.
let listIndexesDefaultOutput =
    assert.commandWorked(db.runCommand({listIndexes: collName})).cursor.firstBatch;
let listIndexesIncludeIndexBuildInfoOutput =
    assert.commandWorked(db.runCommand({listIndexes: collName, includeIndexBuildInfo: true}))
        .cursor.firstBatch;
assertListIndexesOutputsMatch(
    listIndexesDefaultOutput, listIndexesIncludeIndexBuildInfoOutput, ["_id_", doneIndexName], []);

// Ensure that the includeBuildUUIDs flag cannot be set to true at the same time as the
// includeBuildIndexInfo flag, but that cases where they are not both set to true are OK.
assert.commandFailedWithCode(
    db.runCommand({listIndexes: collName, includeIndexBuildInfo: true, includeBuildUUIDs: true}),
    ErrorCodes.InvalidOptions);
const listIndexesIncludeIndexBuildInfoBuildUUIDsFalseOutput =
    assert
        .commandWorked(db.runCommand(
            {listIndexes: collName, includeIndexBuildInfo: true, includeBuildUUIDs: false}))
        .cursor.firstBatch;
assert.eq(listIndexesIncludeIndexBuildInfoBuildUUIDsFalseOutput,
          listIndexesIncludeIndexBuildInfoOutput);
const listIndexesIncludeBuildUUIDsIndexBuildInfoFalseOutput =
    assert
        .commandWorked(db.runCommand(
            {listIndexes: collName, includeIndexBuildInfo: false, includeBuildUUIDs: true}))
        .cursor.firstBatch;
assert.eq(listIndexesIncludeBuildUUIDsIndexBuildInfoFalseOutput, listIndexesDefaultOutput);

// Create a new index, this time intentionally pausing the index build halfway through in order to
// test the in-progress index case.
// For this index build to be resumable, we need to provide a commit quorum of all voting members,
// which is also the default.
const buildingIndexName = "b_1";
IndexBuildTest.pauseIndexBuilds(conn);
const awaitIndexBuild = IndexBuildTest.startIndexBuild(conn,
                                                       coll.getFullName(),
                                                       {b: 1},
                                                       {name: buildingIndexName},
                                                       [],
                                                       /*commitQuorum=*/"votingMembers");
const buildingOpId =
    IndexBuildTest.waitForIndexBuildToScanCollection(db, collName, buildingIndexName);

// Add a non-resumable index build to the listIndexes result by providing a commit quorum of zero,
// though any commit quorum other than the default of all voting members will do the job.
const buildingIndexNameNonResumable = "c_1";
const awaitIndexBuildNonResumable =
    IndexBuildTest.startIndexBuild(conn,
                                   coll.getFullName(),
                                   {c: 1},
                                   {name: buildingIndexNameNonResumable},
                                   [],
                                   /*commitQuorum=*/0);
const buildingOpIdNonResumable =
    IndexBuildTest.waitForIndexBuildToScanCollection(db, collName, buildingIndexNameNonResumable);

// Wait for the new indexes to appear in listIndexes output.
assert.soonNoExcept(() => {
    listIndexesDefaultOutput =
        assert.commandWorked(db.runCommand({listIndexes: collName})).cursor.firstBatch;
    assert(listIndexesDefaultOutput.length == 4);
    return true;
});

// Ensure that the format of the listIndexes output changes as expected in the in-progress index
// case.
listIndexesIncludeIndexBuildInfoOutput =
    assert.commandWorked(db.runCommand({listIndexes: collName, includeIndexBuildInfo: true}))
        .cursor.firstBatch;
assertListIndexesOutputsMatch(
    listIndexesDefaultOutput,
    listIndexesIncludeIndexBuildInfoOutput,
    ["_id_", doneIndexName],
    [buildingIndexName, buildingIndexNameNonResumable],
    {
        [buildingIndexName]: {opid: buildingOpId, resumable: true},
        [buildingIndexNameNonResumable]: {opid: buildingOpIdNonResumable, resumable: false}
    });

IndexBuildTest.resumeIndexBuilds(conn);
IndexBuildTest.waitForIndexBuildToStop(db, collName, buildingIndexNameNonResumable);
awaitIndexBuildNonResumable();
IndexBuildTest.waitForIndexBuildToStop(db, collName, buildingIndexName);
awaitIndexBuild();

// The replication state includes two optional fields for the abort/commit timestamp
// and the abort reason. We can confirm the presence of these fields in the listIndexes
// output using an unique index build that is aborted due to a constraint violation.
assert.commandWorked(coll.insert([{x: 1}, {x: 1}]));
const secondary = rst.getSecondary();
const secondaryColl = secondary.getCollection(coll.getFullName());
const fp = configureFailPoint(secondary, 'hangBeforeCompletingAbort');
try {
    const buildingIndexNameUnique = 'x_1';
    assert.commandFailedWithCode(
        coll.createIndex({x: 1}, {name: buildingIndexNameUnique, unique: true}),
        ErrorCodes.DuplicateKey);
    fp.wait();
    const indexes = IndexBuildTest.assertIndexes(
        secondaryColl,
        5,
        ['_id_', doneIndexName, buildingIndexName, buildingIndexNameNonResumable],
        [buildingIndexNameUnique],
        {includeIndexBuildInfo: true});
    const uniqueIndexBuildInfo = indexes[buildingIndexNameUnique].indexBuildInfo;
    assert(uniqueIndexBuildInfo.hasOwnProperty('replicationState'),
           'unique index info does not contain replicationState: ' + tojson(uniqueIndexBuildInfo));
    const replicationState = uniqueIndexBuildInfo.replicationState;
    assert.eq(replicationState.state,
              'Aborted',
              'Unexpected replication state: ' + tojson(uniqueIndexBuildInfo));
    assert(replicationState.hasOwnProperty('timestamp'),
           'replication state should contain abort timestamp: ' + tojson(uniqueIndexBuildInfo));
    assert.eq(replicationState.code,
              ErrorCodes.IndexBuildAborted,
              'unexpected error code in replication state: ' + tojson(uniqueIndexBuildInfo));
} finally {
    fp.off();
}

rst.stopSet();
})();
