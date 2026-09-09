/**
 * A query using the $assert extension stage errors during pipeline optimization. With query stats
 * sampling at 100% and featureFlagQueryStatsErrors on, that errored read is captured in $queryStats
 * via its query shape key. Reading $queryStats re-parses every stored key; the $assert stage's query
 * shape must be a single-field {$assert: ...} stage so re-parse succeeds.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {
    checkPlatformCompatibleWithExtensions,
    generateExtensionConfigs,
    getExtensionConfDir,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    getQueryStatsServerParameters,
    getQueryStatsWithTransform,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

checkPlatformCompatibleWithExtensions();

// "optimization_transform" defers the $assert error past parse time, so the query shape is
// registered before the command errors as a non-cursor read failure.
const assertPipeline = [
    {$assert: {assertionType: "error", assertInPhase: "optimization_transform"}},
];

describe("query stats re-parse of an errored extension stage", function () {
    let conn;
    let testDB;
    let coll;

    before(function () {
        const extensionNames = generateExtensionConfigs("libextension_errors_mongo_extension.so");

        conn = MongoRunner.runMongod({
            loadExtensions: extensionNames[0],
            extensionsConfigPath: getExtensionConfDir(),
            setParameter: {
                ...getQueryStatsServerParameters().setParameter,
                featureFlagExtensionsAPI: true,
                featureFlagQueryStatsErrors: true,
            },
        });
        assert.neq(null, conn, "failed to start mongod with the extension_errors extension");

        testDB = conn.getDB("test");
        coll = testDB[jsTestName()];
        coll.drop();

        // Start from an empty query stats store so the outcome is deterministic.
        resetQueryStatsStore(conn, "1MB");
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });

    it("re-parses the recorded $assert query shape when reading $queryStats", function () {
        // This aggregate fails during optimization ($assert throws), and the errored read is
        // captured in $queryStats on the mongod.
        assert.throws(() => coll.aggregate(assertPipeline).toArray());

        const stats = getQueryStatsWithTransform(
            testDB,
            {},
            {
                collName: coll.getName(),
                transformIdentifiers: false,
            },
        );

        jsTest.log.info("query stats entries", {stats});

        const assertEntry = stats.find(
            (entry) => entry.key.queryShape.cmdNs.coll === coll.getName(),
        );
        assert(assertEntry, "expected the errored $assert query to be recorded in $queryStats", {
            stats,
        });

        assert.docEq(assertPipeline, assertEntry.key.queryShape.pipeline, stats);
    });
});
