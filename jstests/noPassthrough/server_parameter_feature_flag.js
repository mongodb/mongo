// Test featureFlag enable/disable based on feature flag.
// This test will run whether or not featureFlagToaster is enabled.
// If it is, then we expect the serverParameter to be available and settable.
// If it is not, then we expect no such serverParameter.

(function() {
'use strict';

function runTestForSP(conn) {
    const kFF = 'featureFlagToaster';
    const kSP = 'spTestNeedsFeatureFlagToaster';
    const ff = TestData.setParameters[kFF];
    const assertion = ff ? assert.commandWorked : assert.commandFailed;
    jsTest.log('ServerParameter: ' + kSP);
    jsTest.log(kFF + ': ' + tojson(ff));

    const admin = conn.getDB('admin');
    const initial = assertion(admin.runCommand({getParameter: 1, [kSP]: 1}));
    jsTest.log('Initial: ' + tojson(initial));
    const newval = initial[kSP] + 1;
    const set = assertion(admin.runCommand({setParameter: 1, [kSP]: newval}));
    jsTest.log('Set: ' + tojson(set));
    const changed = assertion(admin.runCommand({getParameter: 1, [kSP]: 1}));
    jsTest.log('Changed: ' + tojson(changed));
    if (ff) {
        assert.neq(initial[kSP], changed[kSP]);
    } else {
        assert.eq(initial.errmsg, 'no option found to get');
        assert.eq(set.errmsg, "Server parameter: '" + kSP + "' is disabled");
        assert.eq(changed.errmsg, 'no option found to get');
    }
}

function runTestForCWSP(conn) {
    const kFF = 'featureFlagClusterWideToaster';
    const kSP = 'cwspTestNeedsFeatureFlagClusterWideToaster';
    const ff = TestData.setParameters[kFF];
    const assertion = ff ? assert.commandWorked : assert.commandFailed;
    jsTest.log('ClusterWideServerParameter: ' + kSP);
    jsTest.log(kFF + ': ' + tojson(ff));

    function val(res) {
        if (!ff) {
            return 0;
        }
        const obj = res.clusterParameters.filter((cwsp) => cwsp._id === kSP)[0];
        return (obj === undefined) ? 0 : obj.intData;
    }

    const admin = conn.getDB('admin');
    const initial = assertion(admin.runCommand({getClusterParameter: kSP}));
    jsTest.log('Initial: ' + tojson(initial));
    const set =
        assertion(admin.runCommand({setClusterParameter: {[kSP]: {intData: val(initial) + 1}}}));
    jsTest.log('Set: ' + tojson(set));
    const changed = assertion(admin.runCommand({getClusterParameter: kSP}));
    jsTest.log('Changed: ' + tojson(changed));
    if (ff) {
        assert.neq(val(initial), val(changed));
    } else {
        const msg = "Server parameter: '" + kSP + "' is disabled";
        assert.eq(initial.errmsg, msg);
        assert.eq(set.errmsg, msg);
        assert.eq(changed.errmsg, msg);
    }
}

{
    jsTest.log('START standalone');
    const standalone = MongoRunner.runMongod({});
    runTestForSP(standalone);
    MongoRunner.stopMongod(standalone);
    jsTest.log('END standalone');
}

{
    jsTest.log('BEGIN sharding');
    const s = new ShardingTest({mongos: 1, config: 1, shards: 3});
    runTestForSP(s.s0);
    runTestForCWSP(s.s0);
    s.stop();
    jsTest.log('END sharding');
}
})();
