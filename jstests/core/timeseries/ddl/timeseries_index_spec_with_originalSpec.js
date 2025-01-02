/**
 * Creates indexes with the originalSpec field and tests that there is appropriate behavior.
 *
 * @tags: [
 *  multiversion_incompatible,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const caseSensitive = {
    locale: "en_US",
    strength: 1,
    caseLevel: true
};
const caseInsensitive = {
    locale: "en_US",
    strength: 2
};

function runTest(coll, bucketsColl, isCaseSensitiveCollation) {
    TimeseriesTest.run(() => {
        // Creating an index without any collation specifications for both the index and the
        // originalSpec field should pass.
        assert.commandWorked(coll.createIndex({x: 1}, {
            originalSpec: {key: {x: "text"}, name: "x_1"},
        }));
        TimeseriesTest.verifyAndDropIndex(
            coll, bucketsColl, /*shouldHaveOriginalSpec=*/ true, "x_1");

        // Creating an index with the same originalSpec collation should pass.
        assert.commandWorked(coll.createIndex({x: 1}, {
            collation: caseInsensitive,
            originalSpec: {key: {x: "text"}, name: "x_1", collation: caseInsensitive},
        }));
        TimeseriesTest.verifyAndDropIndex(
            coll, bucketsColl, /*shouldHaveOriginalSpec=*/ true, "x_1");

        if (isCaseSensitiveCollation) {
            // Creating an index with the same originalSpec collation as the default collation
            // without adding a collation to the index spec should pass.
            assert.commandWorked(coll.createIndex({x: 1}, {
                originalSpec: {key: {x: "text"}, name: "x_1", collation: caseSensitive},
            }));
            TimeseriesTest.verifyAndDropIndex(
                coll, bucketsColl, /*shouldHaveOriginalSpec=*/ true, "x_1");
        } else {
            assert.commandFailedWithCode(coll.createIndex({x: 1}, {
                originalSpec: {key: {x: "text"}, name: "x_1", collation: caseSensitive},
            }),
                                         ErrorCodes.InvalidIndexSpecificationOption);
        }

        if (isCaseSensitiveCollation) {
            // Creating an index with the same index collation as the default collation
            // without adding a collation to the originalSpec should pass.
            assert.commandWorked(coll.createIndex({x: 1}, {
                collation: caseSensitive,
                originalSpec: {
                    key: {x: "text"},
                    name: "x_1",
                },
            }));
            TimeseriesTest.verifyAndDropIndex(
                coll, bucketsColl, /*shouldHaveOriginalSpec=*/ true, "x_1");
        } else {
            assert.commandFailedWithCode(coll.createIndex({x: 1}, {
                collation: caseSensitive,
                originalSpec: {key: {x: "text"}, name: "x_1"},
            }),
                                         ErrorCodes.InvalidIndexSpecificationOption);
        }

        // Creating an index specifying the index's collation and none for the originalSpec's
        // collation should fail if the index's collation is not the default collation.
        assert.commandFailedWithCode(coll.createIndex({x: 1}, {
            collation: (isCaseSensitiveCollation) ? caseInsensitive : caseSensitive,
            originalSpec: {key: {x: "text"}, name: "x_1"},
        }),
                                     ErrorCodes.InvalidIndexSpecificationOption);

        // Creating an index specifying the collation on the originalSpec and none for the
        // index's collation should fail if the originalSpec's collation is not the default
        // collation.
        assert.commandFailedWithCode(coll.createIndex({x: 1}, {
            originalSpec: {
                key: {x: "text"},
                name: "x_1",
                collation: (isCaseSensitiveCollation) ? caseInsensitive : caseSensitive
            },
        }),
                                     ErrorCodes.InvalidIndexSpecificationOption);

        // Creating an index with a different originalSpec collation compared to the index
        // spec collation should fail.
        assert.commandFailedWithCode(coll.createIndex({x: 1}, {
            collation: caseSensitive,
            originalSpec: {key: {x: "text"}, name: "x_1", collation: caseInsensitive},
        }),
                                     ErrorCodes.InvalidIndexSpecificationOption);
    });
}

const collName = jsTestName();
const timeFieldName = "tm";
const metaFieldName = "mm";

// Default collection collation being caseSensitive.
jsTestLog("Running with default collection collation: caseSensitive");
const collWithCollation1 = db.getCollection(collName + '_collation1');
const bucketsCollWithCollation1 = db.getCollection("system.buckets." + collWithCollation1);
collWithCollation1.drop();
assert.commandWorked(db.createCollection(
    collWithCollation1.getName(),
    {timeseries: {timeField: timeFieldName, metaField: metaFieldName}, collation: caseSensitive}));
runTest(collWithCollation1, bucketsCollWithCollation1, /*isCaseSensitiveCollation*/ true);

// Default collection collation being caseInsensitive.
jsTestLog("Running with default collection collation: caseInsensitive");
const collWithCollation2 = db.getCollection(collName + '_collation2');
const bucketsCollWithCollation2 = db.getCollection("system.buckets." + collWithCollation2);
collWithCollation2.drop();
assert.commandWorked(db.createCollection(collWithCollation2.getName(), {
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    collation: caseInsensitive
}));
runTest(collWithCollation2, bucketsCollWithCollation2, /*isCaseSensitiveCollation*/ false);

// No default collection collation.
jsTestLog("Running with no default collection collation");
const collNoCollation = db.getCollection(collName + '_no_collation');
const bucketsCollNoCollation = db.getCollection("system.buckets." + collNoCollation);
collNoCollation.drop();
assert.commandWorked(db.createCollection(
    collNoCollation.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
runTest(collNoCollation, bucketsCollNoCollation, /*isCaseSensitiveCollation*/ false);
