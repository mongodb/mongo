// Test server parameter behavior upon FCV downgrade/upgrade.

(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");

function assertParamExistenceInGetParamStar(output, param, expected) {
    if (output.hasOwnProperty('clusterParameters')) {
        const findRes = output.clusterParameters.find(cp => cp._id === param);
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
    for (let sp of ['spTestNeedsLatestFCV', 'spTestNeedsFeatureFlagBlender']) {
        function assertGetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            assert.eq(err, 'no option found to get');
        }
        function assertSetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            assert.eq(err, "Server parameter: '" + sp + "' is disabled");
        }

        const admin = conn.getDB('admin');
        const initial = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));

        const updateVal = initial[sp] + 1;
        const secondUpdateVal = updateVal + 1;
        assert.commandWorked(admin.runCommand({setParameter: 1, [sp]: updateVal}));

        const changed = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));
        assert.eq(changed[sp], updateVal);
        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getParameter: "*"})), sp, true);

        // Downgrade FCV and ensure we can't get or set if the server knows the FCV (i.e. not
        // mongos)
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
        if (isMongod) {
            assertGetFailed(admin.runCommand({getParameter: 1, [sp]: 1}));
            assertSetFailed(admin.runCommand({setParameter: 1, [sp]: secondUpdateVal}));
        }
        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getParameter: "*"})), sp, !isMongod);

        // Upgrade FCV and ensure get and set work, and that the value is NOT reset to default, as
        // this is a non-cluster parameter
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        const afterUpgrade = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));
        assert.eq(afterUpgrade[sp], updateVal);

        assert.commandWorked(admin.runCommand({setParameter: 1, [sp]: secondUpdateVal}));

        const changedAgain = assert.commandWorked(admin.runCommand({getParameter: 1, [sp]: 1}));
        assert.eq(changedAgain[sp], secondUpdateVal);

        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getParameter: "*"})), sp, true);
    }
}

function runDowngradeUpgradeTestForCWSP(conn, isMongod, isStandalone, verifyStateCallback) {
    for (let sp of ['cwspTestNeedsLatestFCV', 'cwspTestNeedsFeatureFlagBlender']) {
        const admin = conn.getDB('admin');

        function assertGetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            if (isStandalone && !FeatureFlagUtil.isEnabled(admin, 'AuditConfigClusterParameter')) {
                // In this case, we fail earlier with a different error at the command level.
                assert.eq(err, "getClusterParameter cannot be run on standalones");
            } else {
                assert.eq(err, "Server parameter: '" + sp + "' is disabled");
            }
        }

        function assertSetFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            if (isStandalone && !FeatureFlagUtil.isEnabled(admin, 'AuditConfigClusterParameter')) {
                // In this case, we fail earlier with a different error at the command level.
                assert.eq(err, "setClusterParameter cannot be run on standalones");
            } else {
                assert.eq(err, "Server parameter: '" + sp + "' is disabled");
            }
        }

        function val(res) {
            const obj = res.clusterParameters.filter((cwsp) => cwsp._id === sp)[0];
            // Default value for CWSPIntStorage is 0
            return (obj === undefined) ? 0 : obj.intData;
        }

        const initial = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
        const initval = val(initial);

        const updateVal = initval + 1;
        assert.commandWorked(admin.runCommand({setClusterParameter: {[sp]: {intData: updateVal}}}));

        const changed = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
        assert.eq(val(changed), updateVal);
        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getClusterParameter: "*"})), sp, true);
        if (verifyStateCallback !== undefined) {
            verifyStateCallback(sp, true);
        }

        // Downgrade FCV and ensure we can't set, and get either fails (if FCV is known by the
        // server) or gets the default value (if it is not).
        // If our downgrade takes us below the minimum FCV for
        // featureFlagAuditConfigClusterParameter, we expect all cluster parameter commands to fail
        // for standalone.
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
        if (isMongod) {
            assertGetFailed(admin.runCommand({getClusterParameter: sp}));
        } else {
            const afterDowngrade =
                assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
            assert.eq(val(afterDowngrade), initval);
        }
        assertSetFailed(admin.runCommand({setClusterParameter: {[sp]: {intData: updateVal + 1}}}));
        if (!(isStandalone && !FeatureFlagUtil.isEnabled(admin, 'AuditConfigClusterParameter'))) {
            assertParamExistenceInGetParamStar(
                assert.commandWorked(admin.runCommand({getClusterParameter: "*"})), sp, !isMongod);
        }
        if (verifyStateCallback !== undefined) {
            verifyStateCallback(sp, false);
        }

        // Upgrade FCV and ensure get and set work, and that the value is reset to default
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        const afterUpgrade = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
        assert.eq(val(afterUpgrade), initval);

        assert.commandWorked(admin.runCommand({setClusterParameter: {[sp]: {intData: updateVal}}}));

        const changedAgain = assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
        assert.eq(val(changedAgain), updateVal);

        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getClusterParameter: "*"})), sp, true);
    }
}

{
    jsTest.log('START standalone');
    const standalone = MongoRunner.runMongod({setParameter: {featureFlagBlender: true}});
    runDowngradeUpgradeTestForSP(standalone, true /* isMongod */);
    if (FeatureFlagUtil.isEnabled(standalone.getDB('admin'), 'AuditConfigClusterParameter')) {
        // This feature flag enables standalone cluster parameters.
        runDowngradeUpgradeTestForCWSP(standalone, true /* isMongod */, true /* isStandalone */);
    }
    MongoRunner.stopMongod(standalone);
    jsTest.log('END standalone');
}

{
    jsTest.log('START replset');
    const rst =
        new ReplSetTest({nodes: 3, nodeOptions: {setParameter: {featureFlagBlender: true}}});
    rst.startSet();
    rst.initiate();
    runDowngradeUpgradeTestForSP(rst.getPrimary(), true /* isMongod */);
    runDowngradeUpgradeTestForCWSP(rst.getPrimary(), true /* isMongod */, false /* isStandalone */);
    rst.stopSet();
    jsTest.log('END replset');
}

{
    jsTest.log('START sharding');
    const options = {setParameter: {featureFlagBlender: true}};
    const s = new ShardingTest({
        mongos: 1,
        config: 1,
        shards: 3,
        mongosOptions: options,
        configOptions: options,
        shardOptions: options
    });
    function verifyParameterState(sp, expectExists) {
        for (let node of [s.configRS.getPrimary(), s.rs0.getPrimary()]) {
            const out = assert.commandWorked(node.adminCommand({getClusterParameter: "*"}));
            assertParamExistenceInGetParamStar(out, sp, expectExists);

            const cpColl = node.getDB('config').clusterParameters;
            assert.eq(cpColl.find({"_id": sp}).itcount(), expectExists ? 1 : 0);
        }
    }
    // mongos is unaware of FCV
    runDowngradeUpgradeTestForSP(s.s0, false /* isMongod */);
    runDowngradeUpgradeTestForCWSP(
        s.s0, false /* isMongod */, false /* isStandalone */, verifyParameterState);
    s.stop();
    jsTest.log('END sharding');
}
})();
