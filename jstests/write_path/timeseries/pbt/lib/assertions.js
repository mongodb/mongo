/**
 * Assertions PBT
 */

import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

/**
 * Assert that the timeseries collection passes MongoDB's validate() check.
 *
 * @param {DBCollection} tsColl timeseries collection to validate
 */
export function assertCollectionValid(tsColl) {
    const result = tsColl.validate();
    assert(result.valid, () => ({message: "tsColl failed validation", result: result}));
    assert.eq(result.warnings.length, 0, () => ({message: "tsColl validation produced warnings", result: result}));
}

/**
 * Compare the results of a query against a timeseries collection and standard collection which should be identical.
 *
 * @param {DBCollection} tsColl timeseries collection representing "actual" state
 * @param {DBCollection} ctrlColl standard collection uses as control representing "expected" state
 * @param {DBCollection} bucketColl raw timeseries bucket collection
 * @param {Object} [query] query specification
 */
export function assertCollectionsMatch(tsColl, ctrlColl, query = {}) {
    const timeField = tsColl.getMetadata().options.timeseries.timeField;
    const metaField = tsColl.getMetadata().options.timeseries.metaField;
    const tsDocs = tsColl.find(query).sort({_id: 1}).toArray();
    const ctrlDocs = ctrlColl.find(query).sort({_id: 1}).toArray();

    if (tsDocs.length != ctrlDocs.length) {
        jsTest.log.warning("The tsColl and ctrlColl size differs", {
            tsDocLength: tsDocs.length,
            ctrlDocsLength: ctrlDocs.length,
        });
        // push a dummy document into the smaller collection to force a difference
        if (tsDocs.length < ctrlDocs.length) {
            tsDocs.push({});
        } else {
            ctrlDocs.push({});
        }
    }

    for (let i = 0; i < Math.min(tsDocs.length, ctrlDocs.length); ++i) {
        assert.docEq(ctrlDocs[i], tsDocs[i], () => {
            // In the event the documents do not match, find the originating bucket from the
            // timeseries collection and include it in the log.
            let cursor = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl)
                .find({
                    [metaField]: tsDocs[i][metaField],
                    [`control.min.${timeField}`]: {$lte: tsDocs[i][timeField]},
                    [`control.max.${timeField}`]: {$gte: tsDocs[i][timeField]},
                })
                .rawData()
                .limit(1);
            const bucket = cursor.hasNext() ? cursor.next() : null;
            return {
                message: "tsColl and ctrlColl diverged, expected is ctrlDoc, actual is tsDoc",
                documentIndex: i,
                bucket: bucket,
            };
        });
    }
}

export function assertBelowBsonSizeLimit(tsColl) {
    let cursor = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl);
    const bucketDocSizes = cursor
        .aggregate([
            {
                $project: {
                    _id: 1,
                    bsonSize: {$bsonSize: "$$ROOT"},
                },
            },
            {
                $sort: {bsonSize: -1},
            },
        ])
        .toArray();

    const bsonMaxSizeLimit = 16 * 1024 * 1024;
    for (let i = 0; i < bucketDocSizes.length; ++i) {
        assert.lte(bucketDocSizes[i].bsonSize, bsonMaxSizeLimit);
    }
}
