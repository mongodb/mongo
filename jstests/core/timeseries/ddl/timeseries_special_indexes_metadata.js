/**
 * Tests sparse, multikey, wildcard, 2d and 2dsphere indexes on time-series collections
 *
 * Tests index creation, index drops, list indexes, hide/unhide index on a time-series collection.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Time series geo functionality requires optimization.
 *   requires_pipeline_optimization,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    areViewlessTimeseriesEnabled,
    isShardedTimeseries,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

TimeseriesTest.run((insert) => {
    const testdb = db.getSiblingDB(jsTestName());
    const timeseriescoll = testdb.getCollection(jsTestName() + "_coll");

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    /**
     * Sets up an empty time-series collection on namespace 'timeseriescoll' using 'timeFieldName'
     * and 'metaFieldName'.
     */
    function resetCollections() {
        timeseriescoll.drop();

        assert.commandWorked(
            testdb.createCollection(timeseriescoll.getName(), {
                timeseries: {timeField: timeFieldName, metaField: metaFieldName},
            }),
        );
    }

    /**
     * Runs the find cmd directly on the raw timeseries buckets using 'bucketsIndexSpec' as an
     * index hint. Tests that hide() and unhide() of the index allows find to use or fail to use the
     * index, respectively. Tests that the listIndexes cmd returns the expected results from the
     * time-series collection.
     *
     * Some indexes (e.g. wildcard) need queries specified to use an index: the
     * 'timeseriesFindQuery' and 'bucketsFindQuery' can be used for this purpose.
     */
    function hideUnhideListIndexes(
        timeseriesIndexSpec,
        bucketsIndexSpec,
        timeseriesFindQuery = {},
        bucketsFindQuery = {},
    ) {
        jsTestLog(
            "Testing index spec, time-series: " +
                tojson(timeseriesIndexSpec) +
                ", buckets: " +
                tojson(bucketsIndexSpec),
        );

        // Check that the index is usable.
        assert.gt(timeseriescoll.find(timeseriesFindQuery).hint(timeseriesIndexSpec).toArray().length, 0);
        assert.gt(
            getTimeseriesCollForRawOps(timeseriescoll).find(bucketsFindQuery).rawData().hint(bucketsIndexSpec).toArray()
                .length,
            0,
        );

        // Check that listIndexes returns expected results.
        listIndexesHasIndex(timeseriesIndexSpec);

        // Hide the index and check that the find cmd no longer works with the 'bucketsIndexSpec'
        // hint.
        assert.commandWorked(timeseriescoll.hideIndex(timeseriesIndexSpec));
        assert.commandFailedWithCode(
            assert.throws(() => timeseriescoll.find().hint(timeseriesIndexSpec).toArray()),
            ErrorCodes.BadValue,
        );
        assert.commandFailedWithCode(
            assert.throws(() =>
                getTimeseriesCollForRawOps(timeseriescoll).find().rawData().hint(bucketsIndexSpec).toArray(),
            ),
            ErrorCodes.BadValue,
        );

        // Check that the index can still be found via listIndexes even if hidden.
        listIndexesHasIndex(timeseriesIndexSpec);

        // Unhide the index and check that the find cmd with 'bucketsIndexSpec' works again.
        assert.commandWorked(timeseriescoll.unhideIndex(timeseriesIndexSpec));
        assert.gt(timeseriescoll.find(timeseriesFindQuery).hint(timeseriesIndexSpec).toArray().length, 0);
        assert.gt(
            getTimeseriesCollForRawOps(timeseriescoll).find(bucketsFindQuery).rawData().hint(bucketsIndexSpec).toArray()
                .length,
            0,
        );
    }

    /**
     * Checks that listIndexes against the time-series collection returns the 'timeseriesIndexSpec'
     * index. Expects only the 'timeseriesIndexSpec' index to exist (plus an implicitly-created
     * index if the collection is implicitly sharded).
     */
    function listIndexesHasIndex(timeseriesIndexSpec) {
        const timeseriesListIndexesCursor = assert.commandWorked(
            testdb.runCommand({listIndexes: timeseriescoll.getName()}),
        ).cursor;

        // Check the ns is OK.
        const expectedNs = timeseriescoll.getFullName();
        assert.eq(
            expectedNs,
            timeseriesListIndexesCursor.ns,
            "Found unexpected namespace: " + tojson(timeseriesListIndexesCursor),
        );

        // Check for the index.
        const keys = timeseriesListIndexesCursor.firstBatch.map((d) => d.key);
        // An index on {metaField, timeField} gets built by default on time-series collections.
        let expectedKeys = [{mm: 1, tm: 1}];
        if (isShardedTimeseries(timeseriescoll)) {
            expectedKeys.push({tm: 1});
        }
        expectedKeys.push(timeseriesIndexSpec);

        assert.sameMembers(expectedKeys, keys, "Found unexpected index spec: " + tojson(timeseriesListIndexesCursor));
    }

    /**
     * Test sparse index on time-series collection.
     */

    jsTestLog("Testing sparse index on time-series collection.");
    resetCollections();

    // Create a sparse index on the 'mm.tag2' field of the time-series collection.
    const sparseTimeseriesIndexSpec = {[metaFieldName + ".tag2"]: 1};
    const sparseBucketsIndexSpec = {["meta.tag2"]: 1};
    assert.commandWorked(
        timeseriescoll.createIndex(sparseTimeseriesIndexSpec, {sparse: true}),
        "Failed to create a sparse index with: " + tojson(sparseTimeseriesIndexSpec),
    );

    // Only 1 of these 2 documents will be returned by a sparse index on 'mm.tag2'.
    const sparseDocIndexed = {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: {"tag1": "a", "tag2": "b"}};
    const sparseDocNotIndexed = {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: {"tag1": "c"}};
    assert.commandWorked(
        insert(timeseriescoll, sparseDocIndexed),
        "Failed to insert sparseDocIndexed: " + tojson(sparseDocIndexed),
    );
    assert.commandWorked(
        insert(timeseriescoll, sparseDocNotIndexed),
        "Failed to insert sparseDocNotIndexed: " + tojson(sparseDocNotIndexed),
    );

    hideUnhideListIndexes(sparseTimeseriesIndexSpec, sparseBucketsIndexSpec);

    // Check that only 1 of the 2 entries are returned.
    assert.eq(
        1,
        timeseriescoll.find().hint(sparseTimeseriesIndexSpec).toArray().length,
        "Failed to use index: " + tojson(sparseTimeseriesIndexSpec),
    );
    assert.eq(
        1,
        getTimeseriesCollForRawOps(timeseriescoll).find().rawData().hint(sparseBucketsIndexSpec).toArray().length,
        "Failed to use index: " + tojson(sparseBucketsIndexSpec),
    );
    assert.eq(2, timeseriescoll.find().toArray().length, "Failed to see all time-series documents");

    /**
     * Test multikey index on time-series collection.
     */

    jsTestLog("Testing multikey index on time-series collection.");
    resetCollections();

    // Create a multikey index on the time-series collection.
    const multikeyTimeseriesIndexSpec = {[metaFieldName + ".a"]: 1};
    const multikeyBucketsIndexSpec = {["meta.a"]: 1};
    assert.commandWorked(
        timeseriescoll.createIndex(multikeyTimeseriesIndexSpec),
        "Failed to create a multikey index with: " + tojson(multikeyTimeseriesIndexSpec),
    );

    // An index on {a: 1}, where 'a' is an array, will produce a multikey index 'a.zip' and
    // 'a.town'.
    const multikeyDoc = {
        _id: 0,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {
            "a": [
                {zip: "01234", town: "nyc"},
                {zip: "43210", town: "sf"},
            ],
        },
    };
    assert.commandWorked(insert(timeseriescoll, multikeyDoc), "Failed to insert multikeyDoc: " + tojson(multikeyDoc));

    const bucketsFindExplain = assert.commandWorked(
        getTimeseriesCollForRawOps(timeseriescoll).find().rawData().hint(multikeyBucketsIndexSpec).explain(),
    );
    const planStage = getPlanStage(getWinningPlanFromExplain(bucketsFindExplain), "IXSCAN");
    assert.eq(true, planStage.isMultiKey, "Index should have been marked as multikey: " + tojson(planStage));
    assert.eq(
        {"meta.a": ["meta.a"]},
        planStage.multiKeyPaths,
        "Index has wrong multikey paths after insert; plan: " + tojson(planStage),
    );

    hideUnhideListIndexes(multikeyTimeseriesIndexSpec, multikeyBucketsIndexSpec);

    /**
     * Test 2d index on time-series collection.
     */

    jsTestLog("Testing 2d index on time-series collection.");
    resetCollections();

    // Create a 2d index on the time-series collection.
    const twoDTimeseriesIndexSpec = {[metaFieldName]: "2d"};
    const twoDBucketsIndexSpec = {"meta": "2d"};
    assert.commandWorked(
        timeseriescoll.createIndex(twoDTimeseriesIndexSpec),
        "Failed to create a 2d index with: " + tojson(twoDTimeseriesIndexSpec),
    );

    // Insert a 2d index usable document.
    const twoDDoc = {_id: 0, [timeFieldName]: ISODate(), [metaFieldName]: [40, 40]};
    assert.commandWorked(insert(timeseriescoll, twoDDoc), "Failed to insert twoDDoc: " + tojson(twoDDoc));

    assert.eq(
        1,
        getTimeseriesCollForRawOps(timeseriescoll)
            .find({"meta": {$near: [0, 0]}})
            .rawData()
            .toArray().length,
        "Failed to use index: " + tojson(twoDBucketsIndexSpec),
    );

    assert.eq(
        1,
        getTimeseriesCollForRawOps(timeseriescoll)
            .aggregate(
                [{$geoNear: {near: [40.4, -70.4], distanceField: "dist", spherical: true}}, {$limit: 1}],
                kRawOperationSpec,
            )
            .toArray().length,
        "Failed to use 2d index: " + tojson(twoDBucketsIndexSpec),
    );

    assert.eq(
        1,
        timeseriescoll
            .aggregate([
                {
                    $geoNear: {
                        near: [40.4, -70.4],
                        key: metaFieldName,
                        distanceField: "dist",
                        spherical: true,
                    },
                },
                {$limit: 1},
            ])
            .toArray().length,
        "Failed to use 2d index: " + tojson(twoDBucketsIndexSpec),
    );

    hideUnhideListIndexes(twoDTimeseriesIndexSpec, twoDBucketsIndexSpec);

    /**
     * Test 2dsphere index on time-series collection.
     */

    jsTestLog("Testing 2dsphere index on time-series collection.");
    resetCollections();

    // Create a 2dsphere index on the time-series collection.
    const twoDSphereTimeseriesIndexSpec = {[metaFieldName]: "2dsphere"};
    const twoDSphereBucketsIndexSpec = {["meta"]: "2dsphere"};
    assert.commandWorked(
        timeseriescoll.createIndex(twoDSphereTimeseriesIndexSpec),
        "Failed to create a 2dsphere index with: " + tojson(twoDSphereTimeseriesIndexSpec),
    );

    // Insert a 2dsphere index usable document.
    const twoDSphereDoc = {
        _id: 0,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {type: "Point", coordinates: [40, -70]},
    };
    assert.commandWorked(
        insert(timeseriescoll, twoDSphereDoc),
        "Failed to insert twoDSphereDoc: " + tojson(twoDSphereDoc),
    );

    assert.eq(
        1,
        getTimeseriesCollForRawOps(timeseriescoll)
            .aggregate(
                [
                    {
                        $geoNear: {
                            near: {type: "Point", coordinates: [40.4, -70.4]},
                            distanceField: "dist",
                            spherical: true,
                        },
                    },
                    {$limit: 1},
                ],
                kRawOperationSpec,
            )
            .toArray().length,
        "Failed to use 2dsphere index: " + tojson(twoDSphereBucketsIndexSpec),
    );

    assert.eq(
        1,
        timeseriescoll
            .aggregate([
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [40.4, -70.4]},
                        key: metaFieldName,
                        distanceField: "dist",
                        spherical: true,
                    },
                },
                {$limit: 1},
            ])
            .toArray().length,
        "Failed to use 2dsphere index: " + tojson(twoDSphereBucketsIndexSpec),
    );

    hideUnhideListIndexes(twoDSphereTimeseriesIndexSpec, twoDSphereBucketsIndexSpec);

    /**
     * Test wildcard index on time-series collection.
     */

    jsTestLog("Testing wildcard index on time-series collection.");
    resetCollections();

    // Create a wildcard index on the time-series collection.
    const wildcardTimeseriesIndexSpec = {[metaFieldName + ".$**"]: 1};
    const wildcardBucketsIndexSpec = {["meta.$**"]: 1};
    assert.commandWorked(
        timeseriescoll.createIndex(wildcardTimeseriesIndexSpec),
        "Failed to create a wildcard index with: " + tojson(wildcardTimeseriesIndexSpec),
    );

    const wildcard1Doc = {
        _id: 0,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {a: 1, b: 1, c: {d: 1, e: 1}},
    };
    const wildcard2Doc = {
        _id: 1,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {a: 2, b: 2, c: {d: 1, e: 2}},
    };
    const wildcard3Doc = {
        _id: 0,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {a: 3, b: 3, c: {d: 3, e: 3}},
    };
    assert.commandWorked(insert(timeseriescoll, wildcard1Doc));
    assert.commandWorked(insert(timeseriescoll, wildcard2Doc));
    assert.commandWorked(insert(timeseriescoll, wildcard3Doc));

    // Queries on 'metaFieldName' subfields should be able to use the wildcard index hint.
    const wildcardBucketsResults = getTimeseriesCollForRawOps(timeseriescoll)
        .find({"meta.c.d": 1})
        .rawData()
        .hint(wildcardBucketsIndexSpec)
        .toArray();
    assert.eq(2, wildcardBucketsResults.length, "Query results: " + tojson(wildcardBucketsResults));
    const wildcardTimeseriesResults = timeseriescoll
        .find({[metaFieldName + ".c.d"]: 1})
        .hint(wildcardTimeseriesIndexSpec)
        .toArray();
    assert.eq(2, wildcardTimeseriesResults.length, "Query results: " + tojson(wildcardTimeseriesResults));

    hideUnhideListIndexes(
        wildcardTimeseriesIndexSpec,
        wildcardBucketsIndexSpec,
        {[metaFieldName + ".c.d"]: 1} /* timeseriesFindQuery */,
        {"meta.c.d": 1} /* bucketsFindQuery */,
    );

    // Test multikey with a wildcard index on the time-series collection.
    // Wildcard indexes have special handling for multikey.

    jsTestLog("Testing multikey with wildcard index on time-series collection.");

    const wildcardMultikeyDoc = {
        _id: 0,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {
            a: 3,
            b: 3,
            c: {d: 3, e: 3},
            d: [
                {zip: "01234", town: "nyc"},
                {zip: "43210", town: "sf"},
            ],
        },
    };
    assert.commandWorked(timeseriescoll.insert(wildcardMultikeyDoc));

    assert.eq(1, timeseriescoll.find({"mm.d.zip": "01234"}).hint(wildcardTimeseriesIndexSpec).itcount());
    const wildcardFindExplain = assert.commandWorked(
        getTimeseriesCollForRawOps(timeseriescoll)
            .find({"meta.d.zip": "01234"})
            .rawData()
            .hint(wildcardBucketsIndexSpec)
            .explain(),
    );
    const planWildcardStage = getPlanStage(getWinningPlanFromExplain(wildcardFindExplain), "IXSCAN");
    assert(planWildcardStage.isMultiKey, "Index should have been marked as multikey: " + tojson(planWildcardStage));
    assert(
        planWildcardStage.multiKeyPaths.hasOwnProperty("meta.d.zip"),
        "Index has wrong multikey paths after insert; plan: " + tojson(planWildcardStage),
    );
});
