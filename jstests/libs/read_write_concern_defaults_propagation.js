var ReadWriteConcernDefaultsPropagation = (function() {
    "use strict";

    const kDefaultReadConcernField = "defaultReadConcern";
    const kDefaultWriteConcernField = "defaultWriteConcern";
    const kEpochField = "epoch";
    const kSetTimeField = "setTime";
    const kLocalSetTimeField = "localSetTime";

    const kDefaultRWCFields = [kDefaultReadConcernField, kDefaultWriteConcernField];
    const kExtraSetFields = [kEpochField, kSetTimeField];
    const kExtraLocalFields = [kLocalSetTimeField];
    const kExtraFields = [...kExtraSetFields, ...kExtraLocalFields];
    const kSetFields = [...kDefaultRWCFields, ...kExtraSetFields];

    // Check that setting the defaults on setConn propagates correctly across checkConns.
    function setDefaultsAndVerifyPropagation(setConn, checkConns) {
        // Get the current defaults from setConn.
        var initialSetConnDefaults =
            assert.commandWorked(setConn.adminCommand({getDefaultRWConcern: 1}));

        // Ensure that all checkConns agree with this.
        var initialCheckConnsDefaults = checkConns.map(
            conn => assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1})));
        initialCheckConnsDefaults.forEach(
            checkConnDefaults => kSetFields.forEach(
                field => assert.eq(checkConnDefaults[field],
                                   initialSetConnDefaults[field],
                                   "RWC default field " + field + " does not match")));

        // Set new defaults on setConn.
        var newDefaults = {
            defaultReadConcern: {level: "majority"},
            defaultWriteConcern: {w: 2, wtimeout: 0}
        };
        // If these happen to match what's already there, adjust them.
        if (initialSetConnDefaults.defaultReadConcern &&
            friendlyEqual(initialSetConnDefaults.defaultReadConcern,
                          newDefaults.defaultReadConcern)) {
            newDefaults.defaultReadConcern.level = "local";
        }
        if (initialSetConnDefaults.defaultWriteConcern &&
            friendlyEqual(initialSetConnDefaults.defaultWriteConcern,
                          newDefaults.defaultWriteConcern)) {
            newDefaults.defaultWriteConcern.w++;
        }
        var cmd = {setDefaultRWConcern: 1};
        Object.extend(cmd, newDefaults);
        cmd.writeConcern = {
            w: 1
        };  // Prevent any existing default WC (which may be unsatisfiable) from being applied.
        var newDefaultsRes = assert.commandWorked(setConn.adminCommand(cmd));
        kDefaultRWCFields.forEach(field => assert.eq(newDefaultsRes[field],
                                                     newDefaults[field],
                                                     field + " was not set correctly"));
        assert.hasFields(
            newDefaultsRes, kExtraFields, "missing field in result of setDefaultRWConcern");

        // Check that epoch has increased.  Since everything is running on one host, we can also
        // check that each of setTime and localSetTime have increased.
        kExtraFields.forEach(field => {
            if (field in initialSetConnDefaults) {
                assert.gt(newDefaultsRes[field],
                          initialSetConnDefaults[field],
                          field + " did not increase after setting new defaults");
            }
        });

        // Ensure that all checkConns agree with this.
        const timeoutSecs = 2 * 60;
        const intervalSecs = 5;
        var checkConnsDefaults = [];
        assert.soon(
            function() {
                // Get the defaults from all the connections.
                checkConnsDefaults = checkConns.map(
                    conn => assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1})));

                // Check if they all match the recently-set values.
                for (var connDefault of checkConnsDefaults) {
                    for (var field of kSetFields) {
                        if (!friendlyEqual(connDefault[field], newDefaultsRes[field])) {
                            return false;
                        }
                    }

                    // localSetTime reflects which the conn updated its cache.  Since all the conns
                    // (including setConn) are running on a single host, we can check that this is
                    // later than when setDefaultRWConcern was run.
                    if (!(connDefault[kLocalSetTimeField] >= connDefault[kSetTimeField])) {
                        return false;
                    }
                }
                return true;
            },
            () => "updated defaults failed to propagate to all nodes within " + timeoutSecs +
                " secs.  Expected defaults: " + tojson(newDefaultsRes) + ", checkConns: " +
                tojson(checkConns) + ", current state: " + tojson(checkConnsDefaults),
            timeoutSecs * 1000,
            intervalSecs * 1000,
            {runHangAnalyzer: false});
    }

    /**
     * Tests propagation of RWC defaults across an environment.  Updated RWC defaults are set using
     * setConn, and then each connection in the checkConns array is checked to ensure that it
     * becomes aware of the new defaults (within the acceptable window of 2 minutes).
     */
    var runTests = function(setConn, checkConns) {
        // Since these connections are on a brand new replset/cluster, this checks the propagation
        // of the initial setting of defaults.
        setDefaultsAndVerifyPropagation(setConn, checkConns);

        // TODO: remove this after SERVER-43720 is done.
        // Do a dummy write, to bump clusterTime, so that epoch will increase.
        setConn.getCollection("test.dummy").insert({});

        // Do it again to check that updating the defaults also propagates correctly.
        setDefaultsAndVerifyPropagation(setConn, checkConns);
    };

    return {runTests};
})();
