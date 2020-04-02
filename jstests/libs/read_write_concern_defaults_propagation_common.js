var ReadWriteConcernDefaultsPropagation = (function() {
    "use strict";

    const kDefaultReadConcernField = "defaultReadConcern";
    const kDefaultWriteConcernField = "defaultWriteConcern";
    const kUpdateOpTimeField = "updateOpTime";
    const kUpdateWallClockTimeField = "updateWallClockTime";
    const kLocalUpdateWallClockTimeField = "localUpdateWallClockTime";

    const kDefaultRWCFields = [kDefaultReadConcernField, kDefaultWriteConcernField];
    const kExtraSetFields = [kUpdateOpTimeField, kUpdateWallClockTimeField];
    const kExtraLocalFields = [kLocalUpdateWallClockTimeField];
    const kExtraFields = [...kExtraSetFields, ...kExtraLocalFields];
    const kSetFields = [...kDefaultRWCFields, ...kExtraSetFields];

    const timeoutSecs = 2 * 60;
    const intervalSecs = 5;

    // Check that setting the defaults on setConn propagates correctly across checkConns.
    function setDefaultsAndVerifyPropagation(setConn, checkConns, inMemory) {
        // Get the current defaults from setConn.
        var initialSetConnDefaults =
            assert.commandWorked(setConn.adminCommand({getDefaultRWConcern: 1}));

        // Ensure that all checkConns agree with this. Use a loop in case the initial defaults were
        // recently set and have not yet propagated to all nodes.
        var checkConnsDefaults = [];
        var initialCheckConnsDefaults = [];
        assert.soon(
            () => {
                initialCheckConnsDefaults = checkConns.map(
                    conn => assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1})));
                return initialCheckConnsDefaults.every(
                    checkConnDefaults =>
                        kSetFields.every(field => friendlyEqual(checkConnDefaults[field],
                                                                initialSetConnDefaults[field])));
            },
            () => "expected initial defaults to be present on all nodes within" + timeoutSecs +
                " secs.  Expected defaults: " + tojson(initialSetConnDefaults) + ", checkConns: " +
                tojson(checkConns) + ", current state: " + tojson(initialCheckConnsDefaults),
            timeoutSecs * 1000,
            intervalSecs * 1000,
            {runHangAnalyzer: false});

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

        // Check that updateOpTime has increased.  Since everything is running on one host, we can
        // also check that each of kUpdateOpTime and localUpdateWallClockTime have increased.
        kExtraFields.forEach(field => {
            if (field in initialSetConnDefaults) {
                assert.gt(newDefaultsRes[field],
                          initialSetConnDefaults[field],
                          field + " did not increase after setting new defaults");
            }
        });

        // Ensure that all checkConns agree with this.
        assert.soon(
            function() {
                // Get the defaults from all the connections.
                checkConnsDefaults = checkConns.map(conn => assert.commandWorked(conn.adminCommand(
                                                        {getDefaultRWConcern: 1, inMemory})));

                // Check if they all match the recently-set values.
                for (var connDefault of checkConnsDefaults) {
                    if (inMemory) {
                        assert.eq(true, connDefault.inMemory, tojson(connDefault));
                    } else {
                        assert.eq(undefined, connDefault.inMemory, tojson(connDefault));
                    }

                    for (var field of kSetFields) {
                        if (!friendlyEqual(connDefault[field], newDefaultsRes[field])) {
                            return false;
                        }
                    }

                    // localUpdateWallClockTime reflects which the conn updated its cache.  Since
                    // all the conns (including setConn) are running on a single host, we can check
                    // that this is later than when setDefaultRWConcern was run.
                    if (!(connDefault[kLocalUpdateWallClockTimeField] >=
                          connDefault[kUpdateWallClockTimeField])) {
                        return false;
                    }
                }
                return true;
            },
            () => "updated defaults failed to propagate to all nodes within " + timeoutSecs +
                " secs.  Expected defaults: " + tojson(newDefaults) +
                ", checkConns: " + tojson(checkConns) +
                ", current state: " + tojson(checkConnsDefaults) + ", inMemory: " + inMemory,
            timeoutSecs * 1000,
            intervalSecs * 1000,
            {runHangAnalyzer: false});
    }

    /**
     * Tests propagation of RWC defaults across an environment.  Updated RWC defaults are set using
     * setConn, and then each connection in the checkConns array is checked to ensure that it
     * becomes aware of the new defaults (within the acceptable window of 2 minutes).
     */
    var runTests = function(setConn, checkConns, inMemory) {
        // Since these connections are on a brand new replset/cluster, this checks the propagation
        // of the initial setting of defaults.
        setDefaultsAndVerifyPropagation(setConn, checkConns, inMemory);

        // Do it again to check that updating the defaults also propagates correctly.
        setDefaultsAndVerifyPropagation(setConn, checkConns, inMemory);
    };

    /**
     * Asserts eventually all given nodes have no default RWC in their in-memory cache.
     */
    function verifyPropgationOfNoDefaults(checkConns) {
        assert.soon(() => checkConns.every(checkConn => {
            const defaultsRes = assert.commandWorked(
                checkConn.adminCommand({getDefaultRWConcern: 1, inMemory: true}));

            // Note localUpdateWallClockTime is generated by the in-memory cache, so it will be
            // present even if there are no defaults
            const unexpectedFields = kDefaultRWCFields.concat(kExtraSetFields);
            return unexpectedFields.every(field => !defaultsRes.hasOwnProperty(field));
        }),
                    "deleted/dropped defaults failed to propagate to all nodes within " +
                        timeoutSecs + " secs. checkConns: " + tojson(checkConns),
                    timeoutSecs * 1000,
                    intervalSecs * 1000,
                    {runHangAnalyzer: false});
    }

    /**
     * Tests that when the RWC defaults document is removed, either through a delete or drop of
     * config.settings, the RWC defaults cache is invalidated.
     */
    var runDropAndDeleteTests = function(mainConn, checkConns) {
        // Set the defaults to some value other than the implicit server defaults. Then remove the
        // defaults document and verify the cache is invalidated on all nodes.
        setDefaultsAndVerifyPropagation(mainConn, checkConns);
        assert.commandWorked(
            mainConn.getDB("config").settings.remove({_id: "ReadWriteConcernDefaults"}));
        verifyPropgationOfNoDefaults(checkConns);
    };

    return {runTests, runDropAndDeleteTests};
})();
