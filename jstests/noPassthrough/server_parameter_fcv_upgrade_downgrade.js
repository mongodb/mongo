// Test server parameter behavior upon FCV downgrade/upgrade.

(function() {
'use strict';

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

function runDowngradeUpgradeTestForCWSP(conn, isMongod, verifyStateCallback) {
    for (let sp of ['cwspTestNeedsLatestFCV', 'cwspTestNeedsFeatureFlagBlender']) {
        function assertCPFailed(cmd) {
            const err = assert.commandFailed(cmd).errmsg;
            assert.eq(err, "Server parameter: '" + sp + "' is disabled");
        }

        function val(res) {
            const obj = res.clusterParameters.filter((cwsp) => cwsp._id === sp)[0];
            // Default value for CWSPIntStorage is 0
            return (obj === undefined) ? 0 : obj.intData;
        }

        const admin = conn.getDB('admin');
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
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
        if (isMongod) {
            assertCPFailed(admin.runCommand({getClusterParameter: sp}));
        } else {
            const afterDowngrade =
                assert.commandWorked(admin.runCommand({getClusterParameter: sp}));
            assert.eq(val(afterDowngrade), initval);
        }
        assertCPFailed(admin.runCommand({setClusterParameter: {[sp]: {intData: updateVal + 1}}}));
        assertParamExistenceInGetParamStar(
            assert.commandWorked(admin.runCommand({getClusterParameter: "*"})), sp, !isMongod);
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
    runDowngradeUpgradeTestForSP(standalone, true);
    MongoRunner.stopMongod(standalone);
    jsTest.log('END standalone');
}

{
    jsTest.log('START replset');
    const rst =
        new ReplSetTest({nodes: 3, nodeOptions: {setParameter: {featureFlagBlender: true}}});
    rst.startSet();
    rst.initiate();
    runDowngradeUpgradeTestForSP(rst.getPrimary(), true);
    runDowngradeUpgradeTestForCWSP(rst.getPrimary(), true);
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
    runDowngradeUpgradeTestForSP(s.s0, false);
    runDowngradeUpgradeTestForCWSP(s.s0, false, verifyParameterState);
    s.stop();
    jsTest.log('END sharding');
}
})();
