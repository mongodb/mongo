/**
 * Verifies that mongos honors a maxTimeMS nested inside an explained command issued via raw
 * db.runCommand, matching the behavior of a top-level maxTimeMS.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {runWithFailpoint} from "jstests/libs/query/command_diagnostic_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("explain maxTimeMS via raw db.runCommand on mongos", function () {
    const collName = "explain_nested_max_time_ms";
    const verbosity = "executionStats";

    let st;
    let db;

    before(() => {
        st = new ShardingTest({shards: 1, mongos: 1});
        db = st.s.getDB("test");
        assert.commandWorked(db.getCollection(collName).insert([{i: 1}, {i: 2}]));
    });

    after(() => {
        st.stop();
    });

    it("times out for both top-level and nested maxTimeMS", () => {
        // Force any operation that has a deadline set on the mongos to time out immediately.
        runWithFailpoint(st.s, "maxTimeAlwaysTimeOut", {}, () => {
            // Top-level placement times out.
            assert.commandFailedWithCode(
                db.runCommand({explain: {find: collName, filter: {i: 1}}, verbosity, maxTimeMS: 1}),
                ErrorCodes.MaxTimeMSExpired,
            );
            // Nested placement also times out.
            assert.commandFailedWithCode(
                db.runCommand({explain: {find: collName, filter: {i: 1}, maxTimeMS: 1}, verbosity}),
                ErrorCodes.MaxTimeMSExpired,
            );
        });
    });

    it("succeeds when a nested maxTimeMS is not exceeded", () => {
        assert.commandWorked(
            db.runCommand({
                explain: {find: collName, filter: {i: 1}, maxTimeMS: 600000},
                verbosity,
            }),
        );
    });
});
