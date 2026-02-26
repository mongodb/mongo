/**
 * This test confirms that query stats store metrics fields for an update command (where the
 * update modification is specified as a replacement document or pipeline) are correct when
 * inserting a new query stats store entry.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {isUweEnabled} from "jstests/libs/query/uwe_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

/**
 * Run test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 */
function runUpdateCmdMetricsTests(topologyName, setupFn, teardownFn) {
    describe(`query stats update command metrics (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        function resetCollection() {
            coll.drop();
            assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]));
        }

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];

            resetCollection();
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetCollection();
        });

        // TODO(SERVER-119899): Add more assertions once we support returing query stats metrics in response on mongos.
        it("command should work whenquery stats metrics are requested in command", function () {
            const modifierUpdateCommandObj = {
                update: collName,
                updates: [
                    {
                        q: {}, // Should match all docs in collection.
                        u: {$set: {v: "newValue", documentUpdated: true, count: 42}},
                        multi: true,
                        includeQueryStatsMetricsForOpIndex: NumberInt(42),
                    },
                ],
                comment: "running modifier update!!",
            };

            assert.commandWorked(testDB.runCommand(modifierUpdateCommandObj));
        });
    });
}

// Set rate limit and sample rate to 0 to cover the edge case that no requests are sampled while query stats metrics are being requested.
function getSetParametersToSampleNone() {
    return {
        internalQueryStatsRateLimit: 0,
        internalQueryStatsSampleRate: 0,
    };
}

runUpdateCmdMetricsTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({setParameter: getSetParametersToSampleNone()});
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

runUpdateCmdMetricsTests(
    "Sharded",
    () => {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: getSetParametersToSampleNone()},
        });
        const testDB = st.s.getDB("test");
        // TODO SERVER-117919 Remove skipping test due to UWE.
        if (!isUweEnabled(st.s)) {
            st.stop();
            jsTest.log.info("Skipping test: featureFlagUnifiedWriteExecutor is not enabled");
            quit();
        }
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
        return {fixture: st, testDB};
    },
    (st) => st.stop(),
);
