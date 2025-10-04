/**
 * Tests that resumable index builds on time-series collections in the collection scan phase write
 * their state to disk upon clean shutdown and are resumed from the same phase to completion when
 * the node is started back up.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable.
 *   primary_driven_index_builds_incompatible,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const db = rst.getPrimary().getDB(dbName);
const coll = db[jsTestName()];
coll.drop();

const timeFieldName = "time";
const metaFieldName = "m";
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

// The {meta: 1, time: 1} index gets built by default on the time-series collection. This
// test assumes that all of the indexes in the collection are not finished to ensure they are
// resumed when the node is restarted. Drop this index.
assert.commandWorked(coll.dropIndex({[metaFieldName]: 1, [timeFieldName]: 1}));

// Use different metadata fields to guarantee creating three individual buckets in the buckets
// collection.
for (let i = 0; i < 3; i++) {
    assert.commandWorked(
        coll.insert({
            _id: i,
            measurement: "measurement",
            time: ISODate(),
            [metaFieldName]: i,
        }),
    );
}

ResumableIndexBuildTest.run(
    rst,
    dbName,
    coll.getName(),
    [[{[timeFieldName]: 1}]],
    [{name: "hangIndexBuildDuringCollectionScanPhaseAfterInsertion", logIdWithBuildUUID: 20386}],
    /*iteration=*/ 0,
    ["collection scan"],
    [{numScannedAfterResume: 2}],
);

rst.stopSet();
