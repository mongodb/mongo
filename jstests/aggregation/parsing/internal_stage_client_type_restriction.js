/**
 * Verifies that stages registered with AllowedWithClientType::kInternal are rejected when an
 * external client submits them, regardless of whether API strictness is supplied.
 * @tags: [
 *   uses_api_parameters,
 *   requires_timeseries,
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 *   assumes_read_concern_unchanged,
 *   multiversion_incompatible,
 *   # The client-type rejection under test happens at parse time and is tenant-agnostic, so
 *   # multitenancy passthroughs add no coverage for this file.
 *   command_not_supported_in_serverless,
 * ]
 */
import {describe, it, before, after} from "jstests/libs/mochalite.js";

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);
const collName = "testColl";

// assertAllowedInternalIfRequired fires this code for external clients, before and independently
// of any apiStrict check.
const kInternalClientRejectionCode = 5491300;

const kDisallowedStages = ["$_internalListCollections", "$_internalComputeGeoNearDistance"];

function runStageAsExternalClient(stageName, apiParams) {
    return testDB.runCommand(
        Object.assign({aggregate: collName, pipeline: [{[stageName]: {}}], cursor: {}}, apiParams),
    );
}

describe("internal-client-only stage registration", function () {
    before(function () {
        testDB.createCollection(collName);
        assert.commandWorked(testDB[collName].insertOne({a: 1}));
    });

    after(function () {
        testDB.dropDatabase();
    });

    it("rejects stages for external clients without any API parameters", function () {
        for (const stageName of kDisallowedStages) {
            assert.commandFailedWithCode(
                runStageAsExternalClient(stageName, {}),
                kInternalClientRejectionCode,
                {stage: stageName},
            );
        }
    });

    it("rejects stages with apiStrict enabled, proving apiStrict is not the gate", function () {
        for (const stageName of kDisallowedStages) {
            assert.commandFailedWithCode(
                runStageAsExternalClient(stageName, {apiVersion: "1", apiStrict: true}),
                kInternalClientRejectionCode,
                {stage: stageName},
            );
        }
    });

    it("rejects stages nested inside sub-pipelines", function () {
        for (const stageName of kDisallowedStages) {
            assert.commandFailedWithCode(
                testDB.runCommand({
                    aggregate: collName,
                    pipeline: [{$lookup: {from: collName, pipeline: [{[stageName]: {}}], as: "r"}}],
                    cursor: {},
                }),
                kInternalClientRejectionCode,
                {stage: stageName, wrapper: "$lookup"},
            );
            assert.commandFailedWithCode(
                testDB.runCommand({
                    aggregate: collName,
                    pipeline: [{$unionWith: {coll: collName, pipeline: [{[stageName]: {}}]}}],
                    cursor: {},
                }),
                kInternalClientRejectionCode,
                {stage: stageName, wrapper: "$unionWith"},
            );
        }
    });

    it("successfully runs $geoNear on a timeseries collection", function () {
        const tsName = "tsGeo";
        testDB[tsName].drop(); // tolerate test reruns
        testDB.createCollection(tsName, {
            timeseries: {timeField: "time", metaField: "meta"},
        });
        assert.commandWorked(
            testDB[tsName].insertMany([
                {
                    time: ISODate("2026-01-01T00:00:00Z"),
                    meta: 1,
                    loc: {type: "Point", coordinates: [0, 0]},
                },
                {
                    time: ISODate("2026-01-02T00:00:00Z"),
                    meta: 1,
                    loc: {type: "Point", coordinates: [1, 1]},
                },
            ]),
        );
        assert.commandWorked(testDB[tsName].createIndex({loc: "2dsphere"}));

        const result = testDB[tsName]
            .aggregate([
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [0, 0]},
                        distanceField: "dist",
                        key: "loc",
                    },
                },
            ])
            .toArray();
        assert.eq(result.length, 2, {result});
        assert(result[0].dist < result[result.length - 1].dist, {result});
    });
});
