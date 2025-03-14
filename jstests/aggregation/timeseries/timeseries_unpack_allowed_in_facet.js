/**
 * Test that $_internalUnpackBucket is allowed inside facet.
 *
 * @tags: [
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     # TODO (SERVER-88539) the timeseries setup runs a migration. Remove the upgrade-downgrade
 *     # incompatible tag once migrations  work during downgrade.
 *     cannot_run_during_upgrade_downgrade,
 *     requires_fcv_81,
 * ]
 */

const collName = jsTestName();
const tsCollName = jsTestName() + '_with_meta';

function insertData() {
    let doc1 = {
        "ts": ISODate("2024-07-18T00:52:47.169Z"),
        "metafields": {"m": "123456789"},
        "Data": "One"
    };

    let doc2 = {
        "ts": ISODate("2024-07-18T00:52:47.169Z"),
        "metafields": {"m": "123456789"},
        "Data": "Two"
    };

    assert.commandWorked(db.getCollection(tsCollName).insertMany([doc1, doc2]));
    assert.commandWorked(db.getCollection(collName).insertMany([doc1, doc2]));
}

function genPipeline(collection) {
    return [{
        "$facet": {
            "foo": [
                {"$match": {"Data": "One"}},
                {
                    "$lookup": {
                        "from": collection,
                        "as": "moredata",
                        "pipeline": [{"$match": {"m.foo": "123456789"}}]
                    }
                }
            ]
        }
    }];
}

function setup() {
    assert(db[collName].drop());
    assert(db[tsCollName].drop());
    assert.commandWorked(db.createCollection(
        tsCollName, {timeseries: {timeField: "ts", metaField: "m", granularity: "seconds"}}));
    insertData();
}

function assertPipeline(coll, pipeline) {
    const command = {aggregate: coll, cursor: {}};
    command.pipeline = pipeline;

    const res = assert.commandWorked(db.runCommand(command));
    const resultArray = res.cursor.firstBatch;
    assert(resultArray && resultArray.length > 0, "No results returned");
    const facetData = resultArray[0].foo;
    assert(Array.isArray(facetData), "Expected foo facet to be an array");
    assert.eq(1, facetData.length, "Expected exactly one document in the facet output");
    const doc = facetData[0];
    assert.eq("One", doc.Data, "Expected Data field to match 'One'");
}

setup();
jsTestLog("From Normal Collection Lookup To Time Series Collection");
assertPipeline(collName, genPipeline(tsCollName));
jsTestLog("From Time Series Collection Lookup To Normal Collection");
assertPipeline(tsCollName, genPipeline(collName));
jsTestLog("From Time Series Collection Lookup To Time Series Collection");
assertPipeline(tsCollName, genPipeline(tsCollName));
