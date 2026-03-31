/**
 * Tests that $listCatalog stage works with time-series collections.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # In a multitenancy environment the catalog will always return tenant-prefixed entries, and the
 *   # override we use in the multitenancy suites hecks for the absence of this prefix.
 *   simulate_mongoq_incompatible,
 *   # TODO (SERVER-103880) Remoe this tab once getMore is supported in stepdown scenarios.
 *   requires_getmore,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    isViewlessTimeseriesOnlySuite,
    isViewfulTimeseriesOnlySuite,
    isTrackedTimeseries,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

// TODO (SERVER-60746) remove once the topologyTime is guaranteed to be gossiped out to all routers.
TestData.pinToSingleMongos = true;

assert.commandWorked(db.dropDatabase());

const collName = "coll";
const timeFieldName = "timestamp";
const metaFieldName = "metadata";

function validateCollectionMetadata(coll, metadata, onAdminDB) {
    jsTest.log(`Context | nss: ${coll.getFullName()}, tracked: ${isTrackedTimeseries(coll)}, onAdminDB: ${onAdminDB}`);
    jsTest.log(`Collection metadata: ${tojson(metadata)}`);
    if (metadata.name != getTimeseriesBucketsColl(coll).getName()) {
        assert.eq("timeseries", metadata.type);
    } else {
        assert.eq("collection", metadata.type);
    }
    assert.hasFields(metadata.md.options, ["timeseries"]);
    const tsOptions = metadata.md.options.timeseries;

    assert.hasFields(tsOptions, ["timeField", "metaField"]);

    assert.eq(timeFieldName, tsOptions.timeField);
    assert.eq(metaFieldName, tsOptions.metaField);
}

function validateListCatalog(coll, listCatalogRes, onAdminDB, collExists) {
    if (!collExists) {
        assert.eq(
            0,
            listCatalogRes.length,
            `Expected listCatalog output to be empty but found: ${tojson(listCatalogRes)}`,
        );
        return;
    }

    if (!isTrackedTimeseries(coll)) {
        assert.eq(
            1,
            listCatalogRes.length,
            `Expected listCatalog to have exactly one entry for unsharded timeseries collection '${
                collName
            }' but found: ${tojson(listCatalogRes)}`,
        );
        validateCollectionMetadata(coll, listCatalogRes[0], onAdminDB);
    } else {
        listCatalogRes.forEach((listCatalogEntry) => validateCollectionMetadata(coll, listCatalogEntry, onAdminDB));
    }
}
function testListCatalog(coll, collExists) {
    // Check output of listCatalog on admin namespace
    let listCatalogRes = coll
        .getDB()
        .getSiblingDB("admin")
        .aggregate([
            {$listCatalog: {}},
            {
                $match: {
                    "db": coll.getDB().getName(),
                    // TODO(SERVER-120014): Simplify to `name: coll.getName()`.
                    "name": {$in: [coll.getName(), getTimeseriesBucketsColl(coll.getName())]},
                    "md.options.uuid": {$exists: true},
                },
            },
        ])
        .toArray();
    validateListCatalog(coll, listCatalogRes, true /* onAdminDB */, collExists);

    // Check output of listCatalog on collection namespace
    try {
        listCatalogRes = coll.aggregate([{$listCatalog: {}}]).toArray();
        validateListCatalog(coll, listCatalogRes, false /* onAdminDB */, collExists);
    } catch (e) {
        if (e.code != 40602) {
            throw e;
        }

        // Acceptable error: "$listCatalog is only valid as the first stage in a pipeline":
        // $listCatalog fails with this error against a viewful timeseries (but works with viewless timeseries).
        assert(!isViewlessTimeseriesOnlySuite(db));
    }

    if (isViewfulTimeseriesOnlySuite(db)) {
        // Check output of listCatalog on buckets namespace
        listCatalogRes = getTimeseriesBucketsColl(coll)
            .aggregate([{$listCatalog: {}}])
            .toArray();
        validateListCatalog(coll, listCatalogRes, false /* onAdminDB */, collExists);
    }
}

const coll = db[collName];
testListCatalog(coll, false /* collExists */);

assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
testListCatalog(coll, true /* collExists */);

TimeseriesTest.insertManyDocs(coll);
testListCatalog(coll, true /* collExists */);

coll.drop();
testListCatalog(coll, false /* collExists */);
