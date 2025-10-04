// Test server parameter behavior upon FCV downgrade/upgrade.

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertParamExistenceInGetParamStar(output, param, expected) {
    if (output.hasOwnProperty("clusterParameters")) {
        const findRes = output.clusterParameters.find((cp) => cp._id === param);
        if (expected) {
            assert.eq(findRes._id, param);
        } else {
            assert.eq(findRes, undefined);
        }
    } else {
        assert.eq(param in output, expected);
    }
}

function runDowngradeUpgradeTestForSP(conn, isMongod) {
    for (let sp of ["spTestNeedsLatestFCV", "spTestNeedsFeatureFlagBlender"]) {
        function assertGetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            assert.eq(err, "no option found to get");
        }
        function assertSetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            assert.eq(err, "Server parameter: '" + sp + "' is disabled");
        }

        const admin = conn.getDB("admin");
        const initial = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));

        const updateVal = initial[sp] + 1;
        const secondUpdateVal = updateVal + 1;
        assert.commandWorked(admin.runCommand({setParameter: 1, [sp]: updateVal}));

        const changed = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));
        assert.eq(changed[sp], updateVal);
        assertParamExistenceInGetParamStar(assert.commandWorked(admin.runCommand({getParameter: "*"})), sp, true);

        // Downgrade FCV and ensure we can't get or set if the server knows the FCV (i.e. not
        // mongos)
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
        if (isMongod) {
            assertGetFailed(admin.runCommand({getParameter: 1, [sp]: 1}));
            assertSetFailed(admin.runCommand({setParameter: 1, [sp]: secondUpdateVal}));
        }
        assertParamExistenceInGetParamStar(assert.commandWorked(admin.runCommand({getParameter: "*"})), sp, !isMongod);

        // Upgrade FCV and ensure get and set work, and that the value is NOT reset to default, as
        // this is a non-cluster parameter
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        const afterUpgrade = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));
        assert.eq(afterUpgrade[sp], updateVal);

        assert.commandWorked(admin.runCommand({setParameter: 1, [sp]: secondUpdateVal}));

        const changedAgain = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));
        assert.eq(changedAgain[sp], secondUpdateVal);

        assertParamExistenceInGetParamStar(assert.commandWorked(admin.runCommand({getParameter: "*"})), sp, true);
    }
}

function runDowngradeUpgradeTestForCWSP(conn, isMongod, isStandalone, verifyStateCallback) {
    for (let sp of ["cwspTestNeedsLatestFCV", "cwspTestNeedsFeatureFlagBlender"]) {
        const admin = conn.getDB("admin");

        function assertGetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            assert.eq(err, "Server parameter: '" + sp + "' is disabled");
        }

        function assertSetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            assert.eq(err, "Server parameter: '" + sp + "' is disabled");
        }

        function val(res) {
            const obj = res.clusterParameters.filter((cwsp) => cwsp._id === sp)[0];
            // Default value for CWSPIntStorage is 0
            return obj === undefined ? 0 : obj.intData;
        }

        const initial = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
        const initval = val(initial);

        const updateVal = initval + 1;
        assert.commandWorked(admin.runCommand({setClusterParameter: {[sp]: {intData: updateVal}}}));

        assert.soon(() => {
            const changed = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
            return val(changed) == updateVal;
        });
        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getClusterParameter: "*"})),
            sp,
            true,
        );
        if (verifyStateCallback !== undefined) {
            verifyStateCallback(sp, true);
        }

        // Downgrade FCV and ensure we can't set or get.
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        assertGetFailed(admin.runCommand({getClusterParameter: sp}));
        assertSetFailed(admin.runCommand({setClusterParameter: {[sp]: {intData: updateVal + 1}}}));

        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getClusterParameter: "*"})),
            sp,
            false,
        );

        if (verifyStateCallback !== undefined) {
            verifyStateCallback(sp, false);
        }

        // Upgrade FCV and ensure get and set work, and that the value is reset to default
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        const afterUpgrade = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
        assert.eq(val(afterUpgrade), initval);

        assert.commandWorked(admin.runCommand({setClusterParameter: {[sp]: {intData: updateVal}}}));

        assert.soon(() => {
            const changedAgain = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
            return val(changedAgain) == updateVal;
        });

        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getClusterParameter: "*"})),
            sp,
            true,
        );
    }
}

{
    jsTest.log("START standalone");
    const standalone = MongoRunner.runMongod({setParameter: {featureFlagBlender: true}});
    runDowngradeUpgradeTestForSP(standalone, true /* isMongod */);
    runDowngradeUpgradeTestForCWSP(standalone, true /* isMongod */, true /* isStandalone */);
    MongoRunner.stopMongod(standalone);
    jsTest.log("END standalone");
}

{
    jsTest.log("START replset");
    const rst = new ReplSetTest({nodes: 3, nodeOptions: {setParameter: {featureFlagBlender: true}}});
    rst.startSet();
    rst.initiate();
    runDowngradeUpgradeTestForSP(rst.getPrimary(), true /* isMongod */);
    runDowngradeUpgradeTestForCWSP(rst.getPrimary(), true /* isMongod */, false /* isStandalone */);
    rst.stopSet();
    jsTest.log("END replset");
}

{
    jsTest.log("START sharding");
    const options = {setParameter: {featureFlagBlender: true}};
    const s = new ShardingTest({
        mongos: 1,
        config: 1,
        shards: 3,
        mongosOptions: options,
        configOptions: options,
        rsOptions: options,
    });
    function verifyParameterState(sp, expectExists) {
        for (let node of [s.configRS.getPrimary(), s.rs0.getPrimary()]) {
            const out = assert.commandWorked(node.adminCommand({getClusterParameter: "*"}));
            assertParamExistenceInGetParamStar(out, sp, expectExists);

            const cpColl = node.getDB("config").clusterParameters;
            assert.eq(cpColl.find({"_id": sp}).itcount(), expectExists ? 1 : 0);
        }
    }
    // mongos is unaware of FCV
    runDowngradeUpgradeTestForSP(s.s0, false /* isMongod */);
    runDowngradeUpgradeTestForCWSP(s.s0, false /* isMongod */, false /* isStandalone */, verifyParameterState);
    s.stop();
    jsTest.log("END sharding");
}
