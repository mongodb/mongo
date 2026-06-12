/**
 * TEMP DEBUG (do not commit): per-node extensionSearchUsed deltas for $unionWith + $search.
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsInsideHybridSearch,
 *   featureFlagSearchExtension,
 * ]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    createTestView,
    kNumShards,
    kSearchQuery,
    kTestViewName,
} from "jstests/noPassthrough/extensions/ifr_flag_retry_utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

checkPlatformCompatibleWithExtensions();

function perNodeCount(conn) {
    const get = (c) => {
        const ss = c.getDB("admin").runCommand({serverStatus: 1});
        return ss.metrics.extension.search.extensionSearchUsed;
    };
    const counts = {mongos: get(conn)};
    const adminDb = conn.getDB("admin");
    if (FixtureHelpers.isMongos(adminDb)) {
        let i = 0;
        for (const p of FixtureHelpers.getPrimaries(adminDb)) {
            counts["shard" + i++] = get(p);
        }
    }
    return counts;
}

function runTests(conn, shardingTest = null) {
    if (!shardingTest) {
        return; // only interested in sharded
    }
    const {coll} = createTestView(conn, shardingTest);
    const unionWithStage = {
        $unionWith: {coll: coll.getName(), pipeline: [{$search: kSearchQuery}]},
    };
    for (let i = 0; i < 10; i++) {
        const before = perNodeCount(conn);
        coll.aggregate([unionWithStage]).toArray();
        const after = perNodeCount(conn);
        const delta = {};
        for (const k of Object.keys(before)) {
            delta[k] = after[k] - before[k];
        }
        jsTest.log.info("ITERATION " + i + " unionWith-on-coll delta", {delta});
    }
    const unionWithViewStage = {
        $unionWith: {coll: kTestViewName, pipeline: [{$search: kSearchQuery}]},
    };
    for (let i = 0; i < 10; i++) {
        const before = perNodeCount(conn);
        coll.aggregate([unionWithViewStage]).toArray();
        const after = perNodeCount(conn);
        const delta = {};
        for (const k of Object.keys(before)) {
            delta[k] = after[k] - before[k];
        }
        jsTest.log.info("ITERATION " + i + " unionWith-on-view delta", {delta});
    }
}

withExtensions({"libsearch_extension.so": {}}, runTests, ["sharded"], {shards: kNumShards});
