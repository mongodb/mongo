/**
 * Tests that $listExtensions behaves correctly on configurations with and without extensions loaded.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {isLinux} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    generateExtensionConfigs,
    deleteExtensionConfigs,
    generateExtensionConfigWithOptions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test since extensions are only available on Linux platforms.");
    quit();
}

const kNotAllowedWithinFacetErrorCode = 40600;
const kNotAllowedWithinUnionWithErrorCode = 31441;
const kOnlyValidAsFirstStageErrorCode = 40602;

describe("$listExtensions with no extensions loaded", function () {
    function checkZeroResults(conn) {
        const actual = conn
            .getDB("admin")
            .aggregate([{$listExtensions: {}}])
            .toArray();
        const expected = [];
        assert.eq(actual, expected, "result should be empty");
    }

    function checkFailOnNonAdmin(conn) {
        const target = conn.getDB(jsTestName());
        assertErrorCode(
            target,
            [{$listExtensions: {}}],
            ErrorCodes.InvalidNamespace,
            "$listExtensions is not allowed on a non-admin database",
        );
    }

    function checkFailOnCollection(conn) {
        const target = conn.getDB("admin")[jsTestName()];
        assertErrorCode(
            target,
            [{$listExtensions: {}}],
            ErrorCodes.InvalidNamespace,
            "$listExtensions is not allowed on a collection",
        );
    }

    function checkFailOnFacet(conn) {
        const target = conn.getDB("admin");
        assertErrorCode(
            target,
            [
                {
                    $facet: {
                        foo: [{$listExtensions: {}}],
                    },
                },
            ],
            kNotAllowedWithinFacetErrorCode,
            "Using $facet with $listExtensions in sub-pipeline should be rejected",
        );
    }

    function checkFailOnUnionWith(conn) {
        const target = conn.getDB("admin");
        assertErrorCode(
            target,
            [{$unionWith: {pipeline: [{$documents: [{_id: 1}]}, {$listExtensions: {}}]}}],
            kNotAllowedWithinUnionWithErrorCode,
            "Using $unionWith with $listExtensions in sub-pipeline should be rejected",
        );
    }

    function checkRequiredFirstInPipeline(conn) {
        const target = conn.getDB("admin");
        assertErrorCode(
            target,
            [{$listMqlEntities: {entityType: "aggregationStages"}}, {$listExtensions: {}}],
            kOnlyValidAsFirstStageErrorCode,
            "Using $listExtensions as not first in the pipeline should be rejected",
        );
    }

    describe("on standalone", function () {
        before(function () {
            this.standalone = MongoRunner.runMongod();
        });
        it("should return zero results", () => {
            checkZeroResults(this.standalone);
        });
        it("should fail on non-admin database", () => {
            checkFailOnNonAdmin(this.standalone);
        });
        it("should fail on a collection", () => {
            checkFailOnCollection(this.standalone);
        });
        it("should fail within a $facet", () => {
            checkFailOnFacet(this.standalone);
        });
        it("should fail within a $unionWith", () => {
            checkFailOnUnionWith(this.standalone);
        });
        it("should be required to be first in the pipeline", () => {
            checkRequiredFirstInPipeline(this.standalone);
        });
        after(function () {
            MongoRunner.stopMongod(this.standalone);
        });
    });

    describe("on sharded cluster", function () {
        before(function () {
            this.sharded = new ShardingTest({shards: 2, rs: {nodes: 2}, mongos: 1, config: 1});
        });
        it("should return zero results", () => {
            checkZeroResults(this.sharded);
        });
        it("should fail on non-admin database", () => {
            checkFailOnNonAdmin(this.sharded);
        });
        it("should fail on a collection", () => {
            checkFailOnCollection(this.sharded);
        });
        it("should fail within a $facet", () => {
            checkFailOnFacet(this.sharded);
        });
        it("should fail within a $unionWith", () => {
            checkFailOnUnionWith(this.sharded);
        });
        it("should be required to be first in the pipeline", () => {
            checkRequiredFirstInPipeline(this.sharded);
        });
        after(function () {
            this.sharded.stop();
        });
    });
});

describe("$listExtensions with some extensions loaded", function () {
    const libParseExtension = generateExtensionConfigWithOptions(
        "libparse_options_mongo_extension.so",
        "{checkMax: false, max: 1}",
    );
    const libTestOptionsExtension = generateExtensionConfigWithOptions(
        "libtest_options_mongo_extension.so",
        "{optionA: true}",
    );
    const libFooExtension = generateExtensionConfigs("libfoo_mongo_extension.so")[0];
    // The order of extensions is intentionally not alphabetical to verify sorting.
    const extensionsToLoad = [libParseExtension, libFooExtension, libTestOptionsExtension];
    const extOpts = {
        loadExtensions: extensionsToLoad,
    };

    function checkResultsCorrectAndSorted(conn) {
        const actual = conn
            .getDB("admin")
            .aggregate([{$listExtensions: {}}])
            .toArray();
        // TODO(SERVER-111166): Update extensionOptions values after converting YAML to BSON
        // properly.
        const expected = [
            {"extensionName": libFooExtension, "extensionOptions": "{}"},
            {"extensionName": libParseExtension, "extensionOptions": "checkMax: false\nmax: 1"},
            {"extensionName": libTestOptionsExtension, "extensionOptions": "optionA: true"},
        ];
        assert.eq(actual, expected);
    }

    describe("on standalone", function () {
        before(function () {
            this.standalone = MongoRunner.runMongod(extOpts);
        });
        it("should return three results, sorted alphabetically", () => {
            checkResultsCorrectAndSorted(this.standalone);
        });
        after(function () {
            MongoRunner.stopMongod(this.standalone);
        });
    });

    describe("on sharded cluster", function () {
        before(function () {
            this.sharded = new ShardingTest({
                shards: 2,
                rs: {nodes: 2},
                mongos: 1,
                config: 1,
                mongosOptions: extOpts,
                configOptions: extOpts,
                rsOptions: extOpts,
            });
        });
        it("should return three results, sorted alphabetically", () => {
            checkResultsCorrectAndSorted(this.sharded);
        });
        after(function () {
            this.sharded.stop();
        });
    });

    after(function () {
        deleteExtensionConfigs(extensionsToLoad);
    });
});
