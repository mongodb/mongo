/**
 * Tests that the analyze command with mode: "ndv" is rejected on sharded collections.
 *
 * @tags: [
 *   featureFlagPersistentStats,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("analyze mode: ndv on a sharded cluster", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
            rs: {
                nodes: 1,
                setParameter: {
                    featureFlagPersistentStats: true,
                    internalQueryEnablePersistentNDVStats: true,
                },
            },
        });
        this.db = this.st.s.getDB(jsTestName());
    });

    after(function () {
        this.st.stop();
    });

    it("rejects sharded collections", function () {
        const coll = this.db[jsTestName() + "_sharded"];
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(coll.insert({a: 1}));

        assert.commandFailedWithCode(
            this.db.runCommand({analyze: coll.getName(), mode: "ndv", key: "a"}),
            13175803,
        );
    });

    // TODO SERVER-132804: Add positive sharded-context coverage (analyze through mongos on
    // unsharded collections) once cluster_analyze_cmd is no longer test-only (SERVER-124349).
});
