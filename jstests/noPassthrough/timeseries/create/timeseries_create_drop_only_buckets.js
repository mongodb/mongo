/**
 * Tests creating and dropping timeseries bucket collections and view definitions. Tests that we can
 * recover in both create and drop if a partial create occured where we have a bucket collection but
 * no view definition.
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     does_not_support_transactions,
 *     requires_replication,
 *     # Tests a state of partial creation (timeseries buckets exists, timeseries view does not)
 *     # which can't happen with viewless timeseries collections, since they are created atomically.
 *     featureFlagCreateViewlessTimeseriesCollections_incompatible,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const coll = primaryDb[jsTestName()];
const viewName = coll.getName();
const viewNs = coll.getFullName();
const bucketsColl = primaryDb.getCollection("system.buckets." + coll.getName());
const bucketsCollName = bucketsColl.getName();
const timeFieldName = "time";
const expireAfterSecondsNum = 60;

coll.drop();

// Create should create both bucket collection and view
assert.commandWorked(
    primaryDb.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName},
        expireAfterSeconds: expireAfterSecondsNum,
    }),
);
assert.contains(viewName, primaryDb.getCollectionNames());
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Drop should drop both bucket collection and view
assert(coll.drop());
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == viewName),
    -1,
);
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == bucketsCollName),
    -1,
);

// Enable failpoint to allow bucket collection to be created but fail creation of view definition
const failpoint = "failTimeseriesViewCreation";
assert.commandWorked(primaryDb.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn", data: {ns: viewNs}}));
assert.commandFailed(
    primaryDb.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName},
        expireAfterSeconds: expireAfterSecondsNum,
    }),
);
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == viewName),
    -1,
);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Dropping a partially created timeseries where only the bucket collection exists is allowed and
// should clean up the bucket collection
assert(coll.drop());
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == viewName),
    -1,
);
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == bucketsCollName),
    -1,
);

// Trying to create again yields the same result as fail point is still enabled
assert.commandFailed(
    primaryDb.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName},
        expireAfterSeconds: expireAfterSecondsNum,
    }),
);
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == viewName),
    -1,
);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Turn off fail point and test creating view definition with existing bucket collection
assert.commandWorked(primaryDb.adminCommand({configureFailPoint: failpoint, mode: "off"}));

// Different timeField should fail
assert.commandFailed(
    primaryDb.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName + "2"},
        expireAfterSeconds: expireAfterSecondsNum,
    }),
);
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == viewName),
    -1,
);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Different expireAfterSeconds should fail
assert.commandFailed(
    primaryDb.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName},
        expireAfterSeconds: expireAfterSecondsNum + 1,
    }),
);
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == viewName),
    -1,
);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Omitting expireAfterSeconds should fail
assert.commandFailed(primaryDb.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.eq(
    primaryDb.getCollectionNames().findIndex((c) => c == viewName),
    -1,
);
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

// Same parameters should succeed
assert.commandWorked(
    primaryDb.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName},
        expireAfterSeconds: expireAfterSecondsNum,
    }),
);
assert.contains(viewName, primaryDb.getCollectionNames());
assert.contains(bucketsCollName, primaryDb.getCollectionNames());

rst.stopSet();
