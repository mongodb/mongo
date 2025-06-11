/**
 * Tests that running listCollections in rawData mode returns the expected collection options.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {
    kRawOperationSpec,
} from "jstests/core/libs/raw_operation_utils.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const testDB = db.getSiblingDB(jsTestName());

const timeFieldName = 'time';
const coll = testDB.getCollection(jsTestName());
coll.drop();

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

(function assertHasCollectionTypeAndAdditionalProperties() {
    const collectionDocument =
        getTimeseriesCollForDDLOps(testDB, coll).getMetadata(kRawOperationSpec);

    const expectedCollectionDocument = getTimeseriesCollForDDLOps(testDB, coll).getMetadata();
    // In rawData mode, the type is reported as 'collection', rather than 'timeseries'.
    expectedCollectionDocument.type = 'collection';
    // In rawData mode, the clustered index is returned.
    expectedCollectionDocument.options.clusteredIndex = true;

    assert.docEq(expectedCollectionDocument, collectionDocument);
})();

(function assertFoundWhenFilteringByTypeCollection() {
    const collectionDocument =
        assert
            .commandWorked(testDB.runCommand({
                listCollections: 1,
                filter:
                    {type: 'collection', name: getTimeseriesCollForDDLOps(testDB, coll).getName()},
                ...kRawOperationSpec,
            }))
            .cursor.firstBatch[0];

    assert.docEq(getTimeseriesCollForDDLOps(testDB, coll).getMetadata(kRawOperationSpec),
                 collectionDocument);
})();

(function assertLegacyTimeseriesNotAffectedByRawData() {
    if (areViewlessTimeseriesEnabled(testDB)) {
        return;
    }

    // For simplicity, legacy timeseries keep the same behavior for the main (view) namespace.
    assert.docEq(coll.getMetadata(), coll.getMetadata(kRawOperationSpec));
})();

(function assertNonTimeseriesNotAffectedByRawData() {
    // Regular collections and views are unchanged in rawData mode
    const regularCollName = "coll";
    assert.commandWorked(testDB.createCollection(regularCollName));
    assert.docEq(testDB.getCollection(regularCollName).getMetadata(),
                 testDB.getCollection(regularCollName).getMetadata(kRawOperationSpec));

    const viewName = "view";
    assert.commandWorked(testDB.createView(viewName, regularCollName, [{$match: {x: 1}}]));
    assert.docEq(testDB.getCollection(viewName).getMetadata(),
                 testDB.getCollection(viewName).getMetadata(kRawOperationSpec));
})();
