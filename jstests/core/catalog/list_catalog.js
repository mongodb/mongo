/**
 * Basic tests for the $listCatalog aggregation stage.
 *
 * @tags: [
 *     # Time-series collection inserts are not supported in multi-document transactions.
 *     does_not_support_transactions,
 *     # $listCatalog result can be large and may be returned in multiple batches.
 *     requires_getmore,
 *     requires_timeseries,
 * ]
 */
import {
    isViewlessTimeseriesOnlySuite,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

// TODO: SERVER-60746 enable multi-router once $listCatalog is supported on multi-router.
TestData.pinToSingleMongos = true;

const testDB = db.getSiblingDB(jsTestName());
const adminDB = testDB.getSiblingDB("admin");

function checkEntries(coll, entries, type, {numSecondaryIndexes, viewOn}) {
    const ns = coll.getFullName();
    assert(entries.some((entry) => entry.ns === ns));
    for (const entry of entries) {
        if (entry.ns !== ns) {
            continue;
        }

        assert.eq(entry.db, testDB.getName());
        assert.eq(entry.name, coll.getName());
        assert.eq(entry.type, type);
        if (FixtureHelpers.isMongos(testDB)) {
            assert(entry.shard);
        }

        if (type === "timeseries" && entry.viewOn) {
            assert(!isViewlessTimeseriesOnlySuite(testDB));
            assert.eq(entry.viewOn, getTimeseriesBucketsColl(entry.name));
            continue;
        }

        if (type === "view") {
            assert.eq(entry.viewOn, viewOn);
        }

        // Going to check the indexes field.
        //
        // Avoid checking index metadata of unsplittable collections that are not in the proper
        // shard. Unsplittable collections can exist both in the primary shard and in the
        // owning shard local catalog, but createIndexes will only contact the owning shard.
        if (type === "collection" && !FixtureHelpers.isUnsplittable(coll)) {
            // Adding the {_id:1} to the index count.
            let expectedNumIndexes = numSecondaryIndexes + 1;
            if (entry.md.options.clusteredIndex) {
                // The _id clustered index is shown under 'md.options.clusteredIndex' instead of
                // 'md.indexes'.
                --expectedNumIndexes;
            }
            if (FixtureHelpers.isSharded(coll)) {
                // Adding the shard key index to the index count.
                ++expectedNumIndexes;
            }
            if (TestData.implicitWildcardIndexesEnabled) {
                // An implicit wildcard index is created for every secondary index.
                expectedNumIndexes += numSecondaryIndexes;
            }
            assert.eq(expectedNumIndexes, entry.md.indexes.length);
        }
    }
}

describe("Basic tests for the $listCatalog aggregation stage", function () {
    before(() => {
        assert.commandWorked(testDB.dropDatabase());
    });

    it("simple collection with one secondary index", () => {
        const collSimple = testDB.collSimple;
        assert.commandWorked(collSimple.createIndex({a: 1}));
        assert.commandWorked(collSimple.insert({_id: 0, a: 0}));

        // collection-ful aggregate
        {
            const res = collSimple.aggregate([{$listCatalog: {}}]).toArray();
            jsTestLog(collSimple.getFullName() + " $listCatalog: " + tojson(res));
            checkEntries(collSimple, res, "collection", {numSecondaryIndexes: 1});
        }

        // collection-less aggregate
        {
            const res = adminDB.aggregate([{$listCatalog: {}}]).toArray();
            jsTestLog("Collectionless $listCatalog: " + tojson(res));
            checkEntries(collSimple, res, "collection", {numSecondaryIndexes: 1});
        }
    });

    it("simple view", () => {
        const viewSimpleName = "viewSimple";

        const collViewOn = testDB.collViewOn;
        assert.commandWorked(collViewOn.insert({_id: 0, a: 0}));
        assert.commandWorked(testDB.createView(viewSimpleName, collViewOn.getName(), [{$project: {a: 0}}]));
        const viewSimple = testDB.getCollection(viewSimpleName);

        // collection-ful aggregate
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: viewSimpleName, pipeline: [{$listCatalog: {}}], cursor: {}}),
            40602,
        );

        // collection-less aggregate
        const res = adminDB.aggregate([{$listCatalog: {}}]).toArray();
        jsTestLog("Collectionless $listCatalog: " + tojson(res));
        checkEntries(viewSimple, res, "view", {viewOn: collViewOn.getName()});
    });

    it("timeseries collection", () => {
        assert.commandWorked(testDB.createCollection("ts", {timeseries: {timeField: "tt"}}));
        const collTimeseries = testDB.ts;
        assert.commandWorked(collTimeseries.insert({_id: 1, tt: ISODate(), x: 123}));

        // collection-ful aggregate
        try {
            const res = collTimeseries.aggregate([{$listCatalog: {}}]).toArray();
            jsTestLog(collTimeseries.getFullName() + " $listCatalog: " + tojson(res));
            checkEntries(collTimeseries, res, "timeseries", {numSecondaryIndexes: 0});
        } catch (e) {
            if (e.code != 40602) {
                throw e;
            }

            // Acceptable error: "$listCatalog is only valid as the first stage in a pipeline":
            // $listCatalog fails with this error against a viewful timeseries (but works with viewless timeseries).
            assert(!isViewlessTimeseriesOnlySuite(testDB));
        }

        // collection-less aggregate
        const res = adminDB.aggregate([{$listCatalog: {}}]).toArray();
        jsTestLog("Collectionless $listCatalog: " + tojson(res));
        checkEntries(collTimeseries, res, "timeseries", {numSecondaryIndexes: 0});
    });

    it("clustered collection", () => {
        assert.commandWorked(testDB.createCollection("clustered", {clusteredIndex: {key: {_id: 1}, unique: true}}));
        const collClustered = testDB.clustered;
        assert.commandWorked(collClustered.insert({_id: 2, y: "abc"}));

        // collection-ful aggregate
        {
            const res = collClustered.aggregate([{$listCatalog: {}}]).toArray();
            jsTestLog(collClustered.getFullName() + " $listCatalog: " + tojson(res));
            checkEntries(collClustered, res, "collection", {numSecondaryIndexes: 0});
        }

        // collection-less aggregate
        {
            const res = adminDB.aggregate([{$listCatalog: {}}]).toArray();
            jsTestLog("Collectionless $listCatalog: " + tojson(res));
            checkEntries(collClustered, res, "collection", {numSecondaryIndexes: 0});
        }
    });

    it("$listCatalog can't run on a non-admin DB", () => {
        assert.commandFailedWithCode(
            testDB.runCommand({aggregate: 1, pipeline: [{$listCatalog: {}}], cursor: {}}),
            ErrorCodes.InvalidNamespace,
        );
    });
});
