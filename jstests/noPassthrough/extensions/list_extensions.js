/**
 * Tests that $listExtensions outputs loaded extensions.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {isLinux} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// Extensions are only supported on Linux platforms, so skip this test on other operating systems.
if (!isLinux()) {
    quit();
}

// Initializes a replica set test using the provided extension options.
function startReplicaSet(extOpts) {
    const rst = new ReplSetTest({
        nodes: 1,
        nodeOptions: extOpts,
    });
    rst.startSet();
    rst.initiate();
    return rst;
}

describe("$listExtensions with no extensions loaded", function () {
    before(function () {
        this.standalone = MongoRunner.runMongod();
        this.sharded = new ShardingTest({shards: 2, rs: {nodes: 2}, mongos: 1, config: 1});
        this.rst = startReplicaSet();
        this.connections = [this.standalone, this.sharded, this.rst.getPrimary()];
    });

    it("should return zero results", () => {
        this.connections.forEach((conn) => {
            const actual = conn
                .getDB("admin")
                .aggregate([{$listExtensions: {}}])
                .toArray();
            const expected = [];
            assert.eq(actual, expected);
        });
    });

    it("should fail on non-admin database", () => {
        this.connections.forEach((conn) => {
            const target = conn.getDB(jsTestName());
            assertErrorCode(
                target,
                [{$listExtensions: {}}],
                73,
                "$listExtensions must be run against the 'admin' database with {aggregate: 1}",
            );
        });
    });

    it("should fail on a collection", () => {
        this.connections.forEach((conn) => {
            const target = conn.getDB("admin")[jsTestName()];
            assertErrorCode(
                target,
                [{$listExtensions: {}}],
                73,
                "$listExtensions must be run against the 'admin' database with {aggregate: 1}",
            );
        });
    });

    after(function () {
        MongoRunner.stopMongod(this.standalone);
        this.sharded.stop();
        this.rst.stopSet();
    });
});

describe("$listExtensions with some extensions loaded", function () {
    before(function () {
        // TODO(SERVER-110317): Remove getExtensionPath and pass only the extension name to loadExtensions.
        const pathToExtensionFoo = MongoRunner.getExtensionPath("libfoo_mongo_extension.so");
        const pathToExtensionBar = MongoRunner.getExtensionPath("libbar_mongo_extension.so");
        const pathToExtensionVectorSearch = MongoRunner.getExtensionPath("libvector_search_extension.so");
        const extOpts = {
            loadExtensions: [pathToExtensionFoo, pathToExtensionBar, pathToExtensionVectorSearch],
        };
        this.standalone = MongoRunner.runMongod(extOpts);
        this.sharded = new ShardingTest({
            shards: 2,
            rs: {nodes: 2},
            mongos: 1,
            config: 1,
            mongosOptions: extOpts,
            configOptions: extOpts,
            rsOptions: extOpts,
        });
        this.rst = startReplicaSet(extOpts);
        this.connections = [this.standalone, this.sharded, this.rst.getPrimary()];
    });

    it("should return three results, sorted alphabetically", () => {
        this.connections.forEach((conn) => {
            const actual = conn
                .getDB("admin")
                .aggregate([{$listExtensions: {}}])
                .toArray();
            const expected = [
                {"extensionName": "libbar_mongo_extension"},
                {"extensionName": "libfoo_mongo_extension"},
                {"extensionName": "libvector_search_extension"},
            ];
            assert.eq(actual, expected);
        });
    });

    after(function () {
        MongoRunner.stopMongod(this.standalone);
        this.sharded.stop();
        this.rst.stopSet();
    });
});
