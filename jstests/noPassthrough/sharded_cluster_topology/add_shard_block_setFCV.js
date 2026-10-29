/**
 * Test that during an addShard setFeatureCompatibilityVersion commands run via direct connections
 * are blocked on the replica set being added to the cluster.
 *
 * @tags: [
 *   requires_persistence,
 *   multiversion_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

for (const configShard of [false, true]) {
    describe(`addShard blocks setFCV on the new shard [configShard=${configShard}]`, function () {
        beforeEach(function () {
            this.st = new ShardingTest({
                name: jsTestName(),
                shards: 1,
                other: {configShard},
            });

            this.rs0 = new ReplSetTest({name: `${jsTestName()}_rs0`, nodes: 1});
            this.rs0.startSet({shardsvr: ""});
            this.rs0.initiate();
        });

        afterEach(function () {
            this.st.stop();
            this.rs0.stopSet();
        });

        it("blocks setFCV on the replica set being added during addShard", function () {
            jsTest.log.info(
                "Checking that RS is not locked for setting the FCV before running addShard",
            );
            const adminDB = this.rs0.getPrimary().getDB("admin");
            assert.commandWorked(
                adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
            );

            jsTest.log.info(
                "Run an addShard command but pause immediately after blocking setFCV commands",
            );
            const configPrimary = this.st.configRS.getPrimary();
            const addShardFp = configureFailPoint(configPrimary, "hangAfterLockingNewShard");
            const awaitResult = startParallelShell(
                funWithArgs(function (url) {
                    assert.commandWorked(db.adminCommand({addShard: url}));
                }, this.rs0.getURL()),
                this.st.s.port,
            );
            addShardFp.wait();

            jsTest.log.info(
                "Checking that RS is locked for setting the FCV after running addShard",
            );
            assert.commandFailedWithCode(
                adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
                ErrorCodes.ConflictingOperationInProgress,
            );

            addShardFp.off();
            awaitResult();
        });
    });
}
