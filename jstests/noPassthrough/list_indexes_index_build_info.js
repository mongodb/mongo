/**
 * Tests the listIndexes command's includeIndexBuildInfo flag. When the flag is set, all index specs
 * should be contained within the 'spec' subdocument rather than in the document itself, and for
 * indexes which are building, the indexBuildInfo document should be returned next to spec, and
 * should contain a buildUUID.
 *
 * @tags: [requires_replication]
 */

(function() {
'use strict';

load("jstests/noPassthrough/libs/index_build.js");  // for IndexBuildTest

/**
 * Given two listIndexes command results, the first without index build info included and the second
 * with, ensures that ready and in-progress indexes are formatted correctly in each output.
 */
function assertListIndexesOutputsMatch(
    withoutBuildInfo, withBuildInfo, readyIndexNames, buildingIndexNames) {
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
        }
    }
}

const rst = ReplSetTest({nodes: 1});
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
const buildingIndexName = "b_1";
IndexBuildTest.pauseIndexBuilds(conn);
const awaitIndexBuild = IndexBuildTest.startIndexBuild(
    conn, coll.getFullName(), {b: 1}, {name: buildingIndexName}, [], 0);
IndexBuildTest.waitForIndexBuildToStart(db, collName, buildingIndexName);

// Wait for the new index to appear in listIndexes output.
assert.soonNoExcept(() => {
    listIndexesDefaultOutput =
        assert.commandWorked(db.runCommand({listIndexes: collName})).cursor.firstBatch;
    assert(listIndexesDefaultOutput.length == 3);
    return true;
});

// Ensure that the format of the listIndexes output changes as expected in the in-progress index
// case.
listIndexesIncludeIndexBuildInfoOutput =
    assert.commandWorked(db.runCommand({listIndexes: collName, includeIndexBuildInfo: true}))
        .cursor.firstBatch;
assertListIndexesOutputsMatch(listIndexesDefaultOutput,
                              listIndexesIncludeIndexBuildInfoOutput,
                              ["_id_", doneIndexName],
                              [buildingIndexName]);

IndexBuildTest.resumeIndexBuilds(conn);
IndexBuildTest.waitForIndexBuildToStop(db, collName, buildingIndexName);
awaitIndexBuild();
rst.stopSet();
})();
